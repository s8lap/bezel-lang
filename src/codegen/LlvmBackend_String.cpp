#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

llvm::Value* Emitter::loadCodePoint(llvm::Value* strPtr, llvm::Value* idx) {
    auto* i8Ty  = llvm::Type::getInt8Ty(llvm_);
    auto* i16Ty = llvm::Type::getInt16Ty(llvm_);
    llvm::Value* data = loadStringData(strPtr);
    llvm::Value* w = builder_.CreateZExt(loadStringWidth(strPtr), i32(), "cp.w");
    llvm::Value* idx64 = idx->getType() == i64() ? idx
                         : builder_.CreateSExt(idx, i64(), "cp.idx64");
    // #1848: dispatch on the string's per-code-point storage width and load ONLY
    // at the matching width. The earlier form computed all three width loads
    // (i8/i16/i32) speculatively and selected the right one — but for a narrow
    // string the i16/i32 loads read idx*2 / idx*4 bytes into a buffer that only
    // holds idx*width bytes, an out-of-bounds READ past the tail of every short
    // string (e.g. the width-4 load at idx=0 over "ab" reads bytes 0..3 of a
    // 3-byte buffer). The discarded value made it benign in practice, but it is
    // a genuine OOB read (surfaced under generated-code ASan). Branching keeps
    // only the in-bounds load live on each path: idx < len is guaranteed by
    // every caller, so idx*width < len*width ≤ the buffer's byte size.
    auto* w4BB    = llvm::BasicBlock::Create(llvm_, "cp.w4",    currentFn_);
    auto* w2chkBB = llvm::BasicBlock::Create(llvm_, "cp.w2chk", currentFn_);
    auto* w2BB    = llvm::BasicBlock::Create(llvm_, "cp.w2",    currentFn_);
    auto* w1BB    = llvm::BasicBlock::Create(llvm_, "cp.w1",    currentFn_);
    auto* doneBB  = llvm::BasicBlock::Create(llvm_, "cp.done",  currentFn_);

    llvm::Value* is4 = builder_.CreateICmpEQ(w, builder_.getInt32(4), "cp.is4");
    builder_.CreateCondBr(is4, w4BB, w2chkBB);

    // Width=4: ((i32*)data)[idx] → byte offset idx*4.
    builder_.SetInsertPoint(w4BB);
    llvm::Value* off4 = builder_.CreateMul(idx64, builder_.getInt64(4), "cp.o4");
    llvm::Value* ptr4 = builder_.CreateGEP(i8Ty, data, off4, "cp.p4");
    llvm::Value* val4 = builder_.CreateLoad(i32(), ptr4, "cp.v4");
    auto* w4End = builder_.GetInsertBlock();
    builder_.CreateBr(doneBB);

    // Width=2 vs width=1.
    builder_.SetInsertPoint(w2chkBB);
    llvm::Value* is2 = builder_.CreateICmpEQ(w, builder_.getInt32(2), "cp.is2");
    builder_.CreateCondBr(is2, w2BB, w1BB);

    // Width=2: ((i16*)data)[idx] → byte offset idx*2, zero-extended.
    builder_.SetInsertPoint(w2BB);
    llvm::Value* off2 = builder_.CreateMul(idx64, builder_.getInt64(2), "cp.o2");
    llvm::Value* ptr2 = builder_.CreateGEP(i8Ty, data, off2, "cp.p2");
    llvm::Value* val2 = builder_.CreateZExt(
        builder_.CreateLoad(i16Ty, ptr2, "cp.v2"), i32());
    auto* w2End = builder_.GetInsertBlock();
    builder_.CreateBr(doneBB);

    // Width=1: data[idx], zero-extended.
    builder_.SetInsertPoint(w1BB);
    llvm::Value* ptr1 = builder_.CreateGEP(i8Ty, data, idx64, "cp.p1");
    llvm::Value* val1 = builder_.CreateZExt(
        builder_.CreateLoad(i8Ty, ptr1, "cp.v1"), i32());
    auto* w1End = builder_.GetInsertBlock();
    builder_.CreateBr(doneBB);

    builder_.SetInsertPoint(doneBB);
    auto* cp = builder_.CreatePHI(i32(), 3, "cp.result");
    cp->addIncoming(val4, w4End);
    cp->addIncoming(val2, w2End);
    cp->addIncoming(val1, w1End);
    return cp;
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
    //   1. samePtr       (l == r)            → true   [covers null == null]
    //   2. nullCheckBB   (one null, l != r)  → false  [null vs. non-null]
    //   3. lenBB         (sameLen=false)     → false  [different lengths]
    //   4. cmpMergeBB    (same-length)       → fast memcmp (same-width) or
    //                                           per-code-point loop (mixed width)
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

    // Compare byte data, guarding on equal storage width first.
    // Fast path: same width → memcmp(data, data, len*width) == 0 (§4.6).
    // Slow path: different widths → compare one code point at a time (§4.6).
    // Fixes #1479: a width=2 slice of a wide string vs a width=1 literal must
    // compare equal when their code-point sequences are identical; the old
    // memcmp with byteSzL=len*widthL read the wrong number of bytes from the
    // width-1 operand, giving a false "not equal" result.
    builder_.SetInsertPoint(cmpBB);
    llvm::Value* dataL = loadStringData(l);
    llvm::Value* dataR = loadStringData(r);
    llvm::Value* wL8   = loadStringWidth(l);
    llvm::Value* wR8   = loadStringWidth(r);
    llvm::Value* sameW = builder_.CreateICmpEQ(wL8, wR8, "str.eq.samew");
    auto* cmpFastBB  = llvm::BasicBlock::Create(llvm_, "str.eq.fast",  currentFn_);
    auto* cmpSlowBB  = llvm::BasicBlock::Create(llvm_, "str.eq.slow",  currentFn_);
    auto* cmpMergeBB = llvm::BasicBlock::Create(llvm_, "str.eq.merge", currentFn_);
    builder_.CreateCondBr(sameW, cmpFastBB, cmpSlowBB);

    // Fast path: same width — memcmp on raw bytes is correct.
    builder_.SetInsertPoint(cmpFastBB);
    auto* memcmpFn = declareC("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
    llvm::Value* byteSzL = loadStringByteSize(l);
    llvm::Value* fastCmpRes = builder_.CreateCall(memcmpFn, {dataL, dataR, byteSzL});
    llvm::Value* fastEq = builder_.CreateICmpEQ(
        fastCmpRes, llvm::ConstantInt::get(i32(), 0), "str.fasteq");
    auto* cmpFastEnd = builder_.GetInsertBlock();
    builder_.CreateBr(cmpMergeBB);

    // Slow path: different storage widths — decode each operand at its own width.
    // per §4.6: storage width is invisible to the language; comparison is by
    // code-point sequence. Use loadCodePoint to decode each side independently.
    builder_.SetInsertPoint(cmpSlowBB);
    auto* eqIdxS   = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), eqIdxS);
    auto* eqCondBB = llvm::BasicBlock::Create(llvm_, "str.eq.scond", currentFn_);
    auto* eqBodyBB = llvm::BasicBlock::Create(llvm_, "str.eq.sbody", currentFn_);
    auto* eqNextBB = llvm::BasicBlock::Create(llvm_, "str.eq.snext", currentFn_);
    auto* eqNeqBB  = llvm::BasicBlock::Create(llvm_, "str.eq.sneq",  currentFn_);
    auto* eqDoneBB = llvm::BasicBlock::Create(llvm_, "str.eq.sdone", currentFn_);
    builder_.CreateBr(eqCondBB);

    builder_.SetInsertPoint(eqCondBB);
    llvm::Value* eqSi = builder_.CreateLoad(i64(), eqIdxS, "str.eq.si");
    // lenL == lenR was confirmed in lenBB; loop to lenL.
    builder_.CreateCondBr(builder_.CreateICmpSLT(eqSi, lenL), eqBodyBB, eqDoneBB);

    builder_.SetInsertPoint(eqBodyBB);
    llvm::Value* cpL = loadCodePoint(l, eqSi);
    llvm::Value* cpR = loadCodePoint(r, eqSi);
    builder_.CreateCondBr(builder_.CreateICmpEQ(cpL, cpR, "str.eq.cpeq"),
                          eqNextBB, eqNeqBB);

    builder_.SetInsertPoint(eqNextBB);
    builder_.CreateStore(builder_.CreateAdd(eqSi, builder_.getInt64(1)), eqIdxS);
    builder_.CreateBr(eqCondBB);

    builder_.SetInsertPoint(eqNeqBB);
    auto* eqNeqEnd = builder_.GetInsertBlock();
    builder_.CreateBr(cmpMergeBB);

    builder_.SetInsertPoint(eqDoneBB);
    auto* eqDoneEnd = builder_.GetInsertBlock();
    builder_.CreateBr(cmpMergeBB);

    // Merge comparison result from fast and slow paths.
    builder_.SetInsertPoint(cmpMergeBB);
    auto* mergedEq = builder_.CreatePHI(i1(), 3, "str.eq.merged");
    mergedEq->addIncoming(fastEq,               cmpFastEnd);  // same-width memcmp
    mergedEq->addIncoming(builder_.getFalse(),  eqNeqEnd);    // early mismatch
    mergedEq->addIncoming(builder_.getTrue(),   eqDoneEnd);   // all code points equal
    auto* cmpEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);

    // Merge: 4 incoming edges (structure unchanged from original).
    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(i1(), 4, "str.eq");
    phi->addIncoming(builder_.getTrue(),  entryBB);       // samePtr (incl. null==null)
    phi->addIncoming(builder_.getFalse(), nullCheckEnd);  // one side null
    phi->addIncoming(builder_.getFalse(), lenEnd);        // different lengths
    phi->addIncoming(mergedEq,            cmpEnd);        // comparison result
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
    // §18.5 / §2.5 (#1099): a DateTime value with a (non-alignment) format spec
    // — `$"{dt:yyyy-MM-dd}"` — dispatches to the runtime DateTime formatter, the
    // SAME bz_datetime_to_string_fmt that `DateTime.ToString(format)` uses,
    // instead of falling through to the no-arg ToString (which rendered the full
    // "…T00:00:00" form). The alignment path (`,WIDTH:FORMAT`) recurses here with
    // the inner format, so this single branch covers both.
    if (spec[0] != ',' && rt && rt->kind == sema::TypeRef::Kind::Named) {
        std::string_view bn = builtinTypeName(rt);
        // §18.5 / §2.5 / §2.18 (v0.172, #812/#1339; v0.230, #3292): temporal +
        // Guid dispatch. DateTime / DateOnly / TimeOnly format via
        // bz_datetime_to_string_fmt (DateOnly's day-number and TimeOnly's
        // ms-of-day are first widened to DateTime ticks). Per §2.18 v0.230,
        // non-specifier characters are LITERAL separators, but a malformed
        // run of a specifier letter (y/M/d/H/m/s/f) FAULTS as FormatException
        // (strict-fault) — the runtime returns nullptr and the call site
        // raises FormatException. TimeSpan takes only "c"; Guid only "D"/"N";
        // an unrecognized specifier on those raises FormatException
        // (§2.18 strict-fault).
        if (!bn.empty()) {
            llvm::Value* fmtStr = lowerStringLiteral("\"" + spec + "\"", false);
            // §2.18 (v0.230, #3292): bz_datetime_to_string_fmt returns nullptr
            // on a malformed specifier run ("yyy", "MMM", lone "s", …). The
            // interpolation format is a compile-time constant, so we COULD
            // validate it here — but the runtime check is the single source
            // of truth for the rule, and matches the decimal pattern at
            // bz_decimal_to_string_fmt. Defending in depth at the call site
            // means a future caller that bypasses emitWithFormatSpec doesn't
            // re-introduce the silent-pass-through behavior (#3292).
            if (bn == "DateTime" && val->getType()->isIntegerTy(64)) {
                auto* fn = declareC("bz_datetime_to_string_fmt", ptrTy(),
                                    {i64(), ptrTy()});
                llvm::Value* res = builder_.CreateCall(fn, {val, fmtStr}, "dt.fmt");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    res, llvm::Constant::getNullValue(ptrTy()), "dt.fmt.null");
                emitGuardThrow(isNull, "FormatException");
                return res;
            }
            if (bn == "DateOnly" && val->getType()->isIntegerTy()) {
                llvm::Value* dn = val->getType()->isIntegerTy(32)
                    ? val : builder_.CreateTrunc(val, i32());
                llvm::Value* ticks = builder_.CreateMul(
                    builder_.CreateSExt(dn, i64()),
                    builder_.getInt64(864000000000LL));  // TICKS_PER_DAY
                auto* fn = declareC("bz_datetime_to_string_fmt", ptrTy(),
                                    {i64(), ptrTy()});
                llvm::Value* res = builder_.CreateCall(fn, {ticks, fmtStr}, "do.fmt");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    res, llvm::Constant::getNullValue(ptrTy()), "do.fmt.null");
                emitGuardThrow(isNull, "FormatException");
                return res;
            }
            if (bn == "TimeOnly" && val->getType()->isIntegerTy()) {
                llvm::Value* ms = val->getType()->isIntegerTy(32)
                    ? val : builder_.CreateTrunc(val, i32());
                llvm::Value* ticks = builder_.CreateMul(
                    builder_.CreateSExt(ms, i64()),
                    builder_.getInt64(10000LL));          // TICKS_PER_MS
                auto* fn = declareC("bz_datetime_to_string_fmt", ptrTy(),
                                    {i64(), ptrTy()});
                llvm::Value* res = builder_.CreateCall(fn, {ticks, fmtStr}, "to.fmt");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    res, llvm::Constant::getNullValue(ptrTy()), "to.fmt.null");
                emitGuardThrow(isNull, "FormatException");
                return res;
            }
            if (bn == "TimeSpan" && val->getType()->isIntegerTy(64)) {
                if (spec == "c") {
                    auto* fn = declareC("bz_timespan_to_string_c", ptrTy(),
                                        {i64()});
                    return builder_.CreateCall(fn, {val}, "ts.fmt");
                }
                return emitAlwaysFormatException();
            }
            if (bn == "Guid") {
                llvm::StructType* gst = guidStructType();
                llvm::AllocaInst* slot = entryAlloca(gst);
                llvm::Value* gv = val;
                if (gv->getType()->isPointerTy())
                    gv = builder_.CreateLoad(gst, gv, "guid.fmt.load");
                builder_.CreateStore(gv, slot);
                if (spec == "D") {
                    auto* fn = declareC("bz_guid_to_string", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {slot}, "guid.fmtD");
                }
                if (spec == "N") {
                    auto* fn = declareC("bz_guid_to_string_n", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {slot}, "guid.fmtN");
                }
                return emitAlwaysFormatException();
            }
        }
        // §18.5 (v0.172): a user type implementing IFormattable — dispatch to
        // its ToString(format). (bn is empty for a user type, so this is
        // reached only for a non-temporal Named operand.)
        if (rt->declaration) {
            if (llvm::Value* r =
                    emitIFormattableDispatch(val, rt->declaration, spec))
                return r;
        }
        // fix #4328 (§18.5 / stdlib §2.18, v0.298): the BUILT-IN INTERFACE HEAD
        // `IFormattable` (declaration-less Named ref, in `kBuiltinInterfaceHeads`
        // since fix #4326 — `rt->declaration == nullptr` so the arm above
        // correctly rejects, and `isBuiltinInterfaceHead` answers true). §18.5
        // spec-cites this: "String interpolation dispatches a hole's format
        // spec here (stdlib §2.18): `$"{money:C2}"` calls `money.ToString("C2")`
        // when `money`'s type implements `IFormattable`." The proper fix
        // (synthesizing a MethodSymbol for `IFormattable.ToString(format)` so
        // Pass D's itable construction populates a per-(class×IFormattable)
        // itable slot) is the §2.18 follow-up filed alongside this lane
        // (#4328 ships the IDisposable / IAsyncDisposable arms; IFormattable
        // format-spec dispatch through the interface head is the same-shape
        // narrower gap). Pre-fix `#4328` the `M.Money` literal was returned
        // here — wrong but not a crash; the post-fix regression test for the
        // IFormattable half is deferred to the follow-up. // per §18.5,
        // §2.18, fix #4328
    }
    // §18.5 / §9.3 / §2.18 #1412: a decimal value with a format specifier.
    // Dispatches to bz_decimal_to_string_fmt for the full §2.18 decimal vocabulary:
    // F/Fn, E/En, G, P/Pn.  An unrecognized specifier raises FormatException
    // (§2.18 strict-fault); since the specifier is a compile-time constant here,
    // we validate it at codegen time and emit emitAlwaysFormatException() for
    // anything outside the vocabulary — identical to the float/int approach.
    if (spec[0] != ',' && rt && rt->kind == sema::TypeRef::Kind::Primitive &&
        rt->primitive == sema::PrimitiveType::Decimal) {
        char specCh = spec[0];
        char specUp = (specCh >= 'a' && specCh <= 'z') ? (char)(specCh - 32) : specCh;
        // Validate tail: must be all digits (or empty).
        bool tailOk = true;
        for (std::size_t ti = 1; ti < spec.size(); ++ti)
            if (!std::isdigit(static_cast<unsigned char>(spec[ti])))
                { tailOk = false; break; }
        // G takes no count (§2.18: "G" not "Gn" for decimal).
        bool specOk = tailOk &&
                      (specUp == 'F' || specUp == 'E' || specUp == 'P' ||
                       (specUp == 'G' && spec.size() == 1));
        if (!specOk)
            return emitAlwaysFormatException();  // §2.18 strict-fault
        llvm::Value* fmtStr = lowerStringLiteral("\"" + spec + "\"", false);
        llvm::AllocaInst* dslot = entryAlloca(decimalStructType());
        builder_.CreateStore(val, dslot);
        auto* fn = declareC("bz_decimal_to_string_fmt", ptrTy(),
                            {ptrTy(), ptrTy()});
        return builder_.CreateCall(fn, {dslot, fmtStr}, "dec.fmt");
    }
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
        // §8.5/§9.1 (#1283): a Named operand (enum / class / record / interface,
        // or a generic-T that resolves to one) formats via the general
        // value-to-string converter — the SAME path the non-alignment
        // interpolation and record-field formatting use: an enum renders its
        // type name (§8.5), a class its ToString override (else type name), a
        // record its structural ToString, an interface its dynamic ToString.
        // Previously an enum fell to the integer branch below (rendering its
        // underlying value, not its name) and a class/record/interface produced
        // no strVal at all, so the whole alignment was dropped. DateTime/Decimal
        // with an inner `:FORMAT` are handled above; a bare-alignment DateTime
        // (i64) flows through emitValueToString to the same numeric fallback it
        // used before, so this does not change those paths.
        // fix #3442 (§8.5 / §18.5, v0.283): a UNIT operand joins the Named
        // operands in routing through the general converter, for the identical
        // reason — otherwise it falls to the default chain below, where the
        // unit slot's inert placeholder byte matches `isIntegerTy()` and
        // `$"[{u,5}]"` renders `[    0]` instead of `[   ()]`. §18.5's
        // alignment applies to the RENDERED text whatever the type, so the only
        // question is which renderer produces it, and for unit that is §8.5's
        // `"()"` — the same answer the unaligned hole now gets.
        if (!strVal && rt &&
            (rt->kind == sema::TypeRef::Kind::Named ||
             (rt->kind == sema::TypeRef::Kind::Unit && !rt->nullable))) {
            strVal = emitValueToString(val, rt);
        }
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
        llvm::Value* padded = emitSnprintfToString(snpFmt, datPtr);
        // fix #2661 (§18.5 / §28.9, v0.252): DISCHARGE the intermediate render.
        // `emitSnprintfToString` byte-copies `datPtr` into a fresh rc=1 string,
        // so `strVal` is fully consumed here and nothing else references it —
        // but it was never released, so every `$"{x,10}"` hole whose operand
        // produced a MINTED string leaked it. Pre-existing for records and
        // numeric primitives; this fix newly routes spans and `Range<T>`
        // through the same arm, so it is discharged now rather than widened.
        //
        // `emitStringRelease` is rc-aware and a NO-OP on the #422 sentinel, so
        // the literal-form renders (a class/enum type name, `bool`'s
        // "True"/"False") are safe. The ONE shape that must NOT be released is
        // §9.6's identity render — a `string` operand gets ITSELF back, a
        // BORROW of the caller's live value — which is exactly the `isStr`
        // branch above; it is the same carve-out `emitRecordToString`'s field
        // loop makes for a `string` field, and it is tracked explicitly rather
        // than re-derived from the type.
        bool strValIsBorrow = rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                              rt->primitive == P::String;
        if (!strValIsBorrow) emitStringRelease(strVal);
        return padded;
    }
    char specCh = spec[0];
    char kind = std::toupper(static_cast<unsigned char>(specCh));
    int width = (spec.size() > 1) ? std::atoi(spec.c_str() + 1) : -1;
    // §2.18 (v0.172): the count-bearing numeric specifiers are `<letter>` or
    // `<letter><digits>` — the tail after the letter must be all digits. A
    // non-digit tail (e.g. "BOGUS", "Xy") is NOT the letter with a 0 count; it
    // is an unrecognized specifier → FormatException (checked at the else below).
    bool countTailOk = true;
    for (std::size_t ti = 1; ti < spec.size(); ++ti)
        if (!std::isdigit(static_cast<unsigned char>(spec[ti]))) {
            countTailOk = false; break;
        }
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
    // §2.18 (v0.172, #812/#1339): "R" (round-trip) — floating only. Route to the
    // shortest-round-trip renderer, the SAME value no-format ToString() uses.
    if (kind == 'R' && spec.size() == 1 && isFloat) {
        if (val->getType()->isFloatTy()) {
            auto* fn = declareC("bz_float_to_string_r", ptrTy(),
                                {llvm::Type::getFloatTy(llvm_)});
            return builder_.CreateCall(fn, {val}, "flt.r");
        }
        llvm::Value* dv = val->getType()->isDoubleTy()
            ? val : builder_.CreateSIToFP(val, llvm::Type::getDoubleTy(llvm_));
        auto* fn = declareC("bz_double_to_string_r", ptrTy(),
                            {llvm::Type::getDoubleTy(llvm_)});
        return builder_.CreateCall(fn, {dv}, "dbl.r");
    }
    // §2.18 (v0.172): "B"/"Bn" — binary, integers only. Pass the operand's
    // natural-width bit pattern zero-extended to i64 (a signed operand's two's-
    // complement bits print correctly, e.g. int -1 → 32 ones), left-zero-padded
    // to n digits.
    if (kind == 'B' && isInt && countTailOk) {
        llvm::Value* iv = val->getType()->isIntegerTy(64)
            ? val : builder_.CreateZExt(val, i64());
        llvm::Value* md = builder_.getInt32(width >= 0 ? width : 0);
        auto* fn = declareC("bz_ulong_to_binary", ptrTy(), {i64(), i32()});
        return builder_.CreateCall(fn, {iv, md}, "int.bin");
    }
    // F/E/G/P (§18.5) format a numeric value as a real number, so they apply to
    // both floating and integer operands (an int operand is widened to double
    // first — `{42:F2}` → "42.00", issue #739/WR11). X/D apply to integers
    // only. The case of the specifier letter is preserved for E/G/X so an
    // uppercase specifier yields uppercase output (issue #739/WR10, mirroring
    // the existing X/x handling).
    bool useFloatEmit = false;
    double scale = 1.0;  // P (percent) multiplies the value by 100
    if (kind == 'F' && numeric && countTailOk) {
        // §2.18 (v0.172): "F"/"Fn" fixed-point, n decimals, DEFAULT 2.
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "2") + "f";
        useFloatEmit = true;
    } else if (kind == 'E' && numeric && countTailOk) {
        char ech = (specCh == 'e') ? 'e' : 'E';
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "6") + ech;
        useFloatEmit = true;
    } else if (kind == 'G' && numeric && countTailOk) {
        char gch = (specCh == 'g') ? 'g' : 'G';
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "6") + gch;
        useFloatEmit = true;
    } else if (kind == 'P' && numeric && countTailOk) {
        // §18.5 normative example: `{ratio:P1}` → "85.0%". Scale by 100, format
        // fixed-point to the requested digit count (default 2), append a
        // literal '%' (snprintf renders "%%" as one '%'). Issue #739/WR9.
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "2") + "f%%";
        useFloatEmit = true;
        scale = 100.0;
    } else if (kind == 'X' && isInt && countTailOk) {
        // §18.5 X[n]: zero-padded hex to n digits minimum. The value is widened
        // to i64 below, so the conversion MUST carry the `ll` length modifier —
        // a bare `%X`/`%x` reads only the low 32 bits of the i64 argument
        // (#1123: `4294967296:X` printed "0", ulong-max printed "FFFFFFFF").
        char xch = (specCh == 'x') ? 'x' : 'X';
        snprintfFmt = width >= 0
            ? ("%0" + std::to_string(width) + "ll" + xch)
            : (std::string("%ll") + xch);
    } else if (kind == 'D' && isInt && countTailOk) {
        // §18.5 D[n]: zero-padded decimal to n digits minimum. Carry the `ll`
        // length modifier (#1123) so the full 64-bit value is read, and select
        // the unsigned conversion `llu` for unsigned operands so a large
        // unsigned value is not rendered as negative.
        bool unsD = rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                    isUnsignedPrim(rt->primitive);
        const char* dconv = unsD ? "llu" : "lld";
        snprintfFmt = width >= 0
            ? ("%0" + std::to_string(width) + dconv)
            : (std::string("%") + dconv);
    } else if (numeric) {
        // §18.5 / §2.18 (v0.172): the operand is intrinsically formattable
        // (an int/float/decimal — sema admitted the spec) but the specifier is
        // not in this type's vocabulary → the defined FormatException fault,
        // catchable (the specifier is a compile-time constant here).
        return emitAlwaysFormatException();
    } else {
        return nullptr;  // non-numeric operand — caller falls back
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
        // #1123: widen to i64 by the operand's signedness. For `X` (hex), format
        // the operand's natural-width BIT PATTERN — zero-extend so a sub-32-bit
        // unsigned value is not sign-extended (`byte 255:X2` must be "FF", not
        // "FFFFFFFF"); for a signed type the i8/i32 bits are the two's-complement
        // pattern already, and zero-extension preserves exactly those bits (e.g.
        // `int -1:X` → "FFFFFFFF"). For `D` (decimal), preserve the signed value:
        // sign-extend signed operands, zero-extend unsigned ones (paired with the
        // `llu` conversion above).
        bool zeroExt = (kind == 'X') ||
                       (rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                        isUnsignedPrim(rt->primitive));
        llvm::Value* iv = val->getType()->isIntegerTy(64) ? val
            : (zeroExt ? builder_.CreateZExt(val, i64())
                       : builder_.CreateSExt(val, i64()));
        return emitSnprintfToString(snprintfFmt, iv);
    }
}


// §18.5 (v0.184, #1691): apply ONLY the alignment portion of an encoded
// InterpPart::formatSpec to an already-rendered bz_string. The null-hole
// path renders "null" directly (no ToString/ToString(format) dispatch is
// performed on a null value — §18.5 v0.184), but "[t]he alignment applies
// to the rendered text `null` exactly as to any other rendered text (it
// participates in field padding)". This mirrors the padding half of the
// `spec[0] == ','` branch above (snprintf `%Ns` / `%-Ns`) without the
// value-to-string conversion, since the text is already rendered.
llvm::Value* Emitter::emitAlignOnly(llvm::Value* strVal, const std::string& spec) {
    if (!strVal || spec.empty() || spec[0] != ',') return strVal;
    size_t colonPos = spec.find(':', 1);
    std::string widthPart = (colonPos == std::string::npos)
        ? spec.substr(1) : spec.substr(1, colonPos - 1);
    int alignWidth = std::atoi(widthPart.c_str());
    bool leftAlign = alignWidth < 0;
    int w = leftAlign ? -alignWidth : alignWidth;
    if (w == 0) return strVal;
    std::string snpFmt = leftAlign
        ? ("%-" + std::to_string(w) + "s")
        : ("%" + std::to_string(w) + "s");
    llvm::Value* datPtr = builder_.CreateLoad(
        ptrTy(), builder_.CreateStructGEP(stringType(), strVal, 0));
    return emitSnprintfToString(snpFmt, datPtr);
}


// §18.5 (v0.172, #812/#1339): dispatch a hole's format spec to a user type's
// IFormattable.ToString(format): string. Walks the class chain for a
// `ToString` taking one parameter (the format string). Returns null if no such
// method exists (the caller then falls back). emitMethodCall handles both a
// class (pointer) and a record (value) receiver.
llvm::Value* Emitter::emitIFormattableDispatch(llvm::Value* val,
                                               const sema::TypeSymbol* ts,
                                               const std::string& spec) {
    if (!ts || !val) return nullptr;
    sema::MethodSymbol* toStr = nullptr;
    for (const sema::TypeSymbol* cur = ts; cur && !toStr;) {
        for (auto* sym : cur->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
            if (ms && ms->name == "ToString" && ms->params.size() == 1) {
                toStr = ms; break;
            }
        }
        const sema::TypeSymbol* nxt = nullptr;
        for (auto* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
        cur = nxt;
    }
    if (!toStr) return nullptr;
    // emitMethodCall requires a POINTER receiver when the callee's `this` is a
    // pointer (class, and a record's `this` too, §CG29). Spill a record's struct
    // value to a stack slot and pass its address; a class receiver is already a
    // pointer.
    llvm::Value* recv = val;
    if (recv->getType()->isStructTy()) {
        llvm::AllocaInst* slot = entryAlloca(recv->getType());
        builder_.CreateStore(recv, slot);
        recv = slot;
    }
    llvm::Value* fmtStr = lowerStringLiteral("\"" + spec + "\"", false);
    std::vector<llvm::Value*> a{recv, fmtStr};
    return emitMethodCall(toStr, a, false);
}


// §18.5 (v0.172): raise a catchable FormatException for an unrecognized
// specifier on an intrinsically-formattable operand. The specifier is a
// compile-time constant in interpolation, so the throw is unconditional; a
// true-guarded throw keeps the CFG well-formed (the continuation is dead but
// valid) and a surrounding try/catch catches it via the unwind edge.
llvm::Value* Emitter::emitAlwaysFormatException() {
    emitGuardThrow(builder_.getTrue(), "FormatException");
    return lowerStringLiteral("\"\"", false);
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
    // Build format string global.
    auto* fmtData = llvm::ConstantDataArray::getString(llvm_, fmt, true);
    unsigned fid = stringLiteralIdx_++;
    auto* fmtGV = new llvm::GlobalVariable(
        *module_, fmtData->getType(), true,
        llvm::GlobalValue::PrivateLinkage, fmtData,
        ".bz.fmtspec." + std::to_string(fid));
    // per §18.5 (#1186): an alignment width or numeric format spec can render
    // arbitrarily wide (e.g. $"{x,1000000000}" or $"{5:D100}").  A fixed-size
    // stack buffer would over-read when snprintf's *untruncated* return length
    // exceeds the buffer (SIGSEGV, or a stack-byte disclosure at sub-crash
    // widths).  Render with the standard two-pass idiom into a heap buffer
    // sized exactly to the reported length: a sizing call with a null buffer
    // and size 0 yields the would-be length, then a second snprintf renders
    // the full, NUL-terminated result fully in-bounds.
    llvm::Value* wr = builder_.CreateCall(
        snpFn, {llvm::ConstantPointerNull::get(ptrTy()),
                builder_.getInt64(0), fmtGV, val});
    llvm::Value* l64 = builder_.CreateSExt(wr, i64());
    // Build heap-backed bz_string: malloc len+1, render into it (snprintf
    // writes len chars plus the NUL terminator within this exact bound).
    llvm::Value* dSz = builder_.CreateAdd(l64, builder_.getInt64(1));
    llvm::Value* dc = builder_.CreateCall(mallocFn, {dSz});
    builder_.CreateCall(snpFn, {dc, dSz, fmtGV, val});
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
        // Fallback (should not be reached for a well-typed Object): type
        // name. fix #5186 (spec §8.1, v0.311): the §8.5 name-only default
        // for a class renders the qualified name — `"System.Object"` —
        // not the bare identifier.
        return lowerStringLiteral("\"System.Object\"", false);
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

    // #1282 (§9.5 / §4.6): char.ToString() / char interpolation. A char is a
    // Unicode scalar (i32 code point up to U+10FFFF); the snprintf `%c` path
    // below truncated it to its low byte, corrupting any char above U+00FF.
    // Build a single-code-point string at the narrowest storage width via the
    // runtime, preserving the full code point (output UTF-8-encodes it).
    if (prim == P::Char) {
        auto* fn = declareC("bz_char_to_string", ptrTy(), {i32()});
        return builder_.CreateCall(fn, {val}, "char.tostr");
    }

    // §2.18 (v0.172, #812/#1339): the floating no-format ToString() round-trip
    // guarantee — produce the shortest string that reparses to the identical
    // bit pattern (the SAME value the "R" specifier uses). Route double/float
    // to the runtime shortest-round-trip renderer instead of a fixed-precision
    // "%g" (which lost a digit: 1.0/3.0 → "0.333333333333333", not round-trip).
    if (prim == P::Double) {
        auto* fn = declareC("bz_double_to_string_r", ptrTy(),
                            {llvm::Type::getDoubleTy(llvm_)});
        return builder_.CreateCall(fn, {val}, "dbl.r0");
    }
    if (prim == P::Float) {
        auto* fn = declareC("bz_float_to_string_r", ptrTy(),
                            {llvm::Type::getFloatTy(llvm_)});
        return builder_.CreateCall(fn, {val}, "flt.r0");
    }

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
        // P::Char is handled by the bz_char_to_string short-circuit above
        // (#1282) — it never reaches this snprintf path.
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
    // fix #4487 (stdlib §9.6 + spec §4.6, v0.306): the ONE ordinal string
    // comparison. Spec §4.6 defines `<`/`<=`/`>`/`>=` on `string` as "ordinal
    // lexicographic comparison of the CODE-POINT sequence", and stdlib §9.6
    // gives `CompareTo` the same ordinal relation, so this function and the
    // two `CompareTo` arities must answer identically — they now share one
    // runtime implementation, `bz_str_compare_cp`.
    //
    // What stood here was a two-path inline lowering: a `memcmp(dataL, dataR,
    // minLen * width)` fast path when the two operands had the SAME storage
    // width, and a per-code-point loop only when the widths DIFFERED (#1480's
    // fix). The fast path is correct only at width 1. At width 2/4 `memcmp`
    // orders by the LITTLE-ENDIAN storage bytes — low byte first — which §4.6
    // forbids outright, since the per-string width is an implementation detail
    // no result may depend on. Measured on HEAD (4d9d186a) before this fix:
    //
    //     "日本".CompareTo("日x")  -> NEGATIVE (U+672C > U+0078, so it must
    //                                         be POSITIVE)
    //     "Āa" < "ĀĀ"              -> false (0x61 < 0x100, so it must be true)
    //
    // Only the SIGN of the first is a reproducible measurement, and the sign
    // is the whole defect. glibc's `memcmp` may return the byte difference or
    // merely the sign, and the two measurements of this program disagree: -76
    // on the box it was first reproduced on (the analytic difference
    // 0x2C - 0x78) and -1 on a reviewer's rebuild. Do not cite a magnitude —
    // stdlib §2.8 does not specify one.
    //
    // The #1480 tests kept passing because they only ever compared operands of
    // DIFFERENT widths (which took the correct slow path) or differed at an
    // ASCII position (where byte order and code-point order agree).
    //
    // Repairing the fast path is not the fix: it would leave a second copy of
    // the ordering rule in the backend beside the runtime's, which is the
    // #2590 drift this issue exists to close. Routing through the runtime also
    // deletes six basic blocks from every string comparison site.
    //
    // MAGNITUDE: the helper returns -1/0/+1 rather than the byte/code-point
    // difference this used to produce. stdlib §2.8 specifies the SIGN only
    // ("a negative number if a < b, zero if equal, a positive number if a >
    // b"), and every caller here consumes it through a comparison against 0.
    //
    // Null operands: §4.6 requires non-nullable ordering operands, so a
    // well-typed program never passes null. `bz_str_compare_cp` nonetheless
    // treats null as the empty string (it inherits `bz_string_compare`'s
    // documented null handling), where this lowering would have dereferenced
    // it — strictly more defensive, and no well-typed program can observe it.
    auto* fn = declareC("bz_str_compare_cp", i32(), {ptrTy(), ptrTy(), i32()});
    return builder_.CreateCall(fn, {l, r, builder_.getInt32(0)}, "str.ord");
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
        // #2614 / §7.4: unwrap a flow-narrowed `int?` argument — which sema
        // correctly ADMITS as the non-nullable `int` parameter — to its bare
        // payload before the caller trunc/sext-es it to the width it needs
        // (e.g. Insert's `sext ... to i64`). No-op for a plain int.
        //
        // ⚠ This helper is NOT the single funnel for the string int-parameter
        // family: it has six uses (AsSpan's start/length, Substring pos 0,
        // Insert pos 0, Remove pos 0). PadLeft/PadRight/PadCenter's width
        // (~:2152) and padChar, Remove's count (~:2446), Repeat's n (~:2173),
        // Substring's length (~:1868) and IndexOf's startIndex (~:1744/:1794)
        // still lower via a raw `lowerExpr` and retain the #2614 defect — so
        // `s.Substring(n)` compiles while `s.Substring(0, n)` still ICEs.
        // Audited with per-site file:line and per-site confidence in #2625.
        //
        // ⚠ SOUNDNESS PRECONDITION: this unwrap tests the tagged-optional
        // SHAPE, not that sema proved the value non-null, so it is safe ONLY
        // where a sema gate rejects the un-narrowed form. Every method reaching
        // it is gated in checkBuiltinStringCallArgs' `intPos` set
        // (ExprTypecheck_Calls.cpp) — AsSpan was added there by this same fix,
        // precisely because it was NOT gated and the unwrap turned a clean ICE
        // into a silent wrong value. Do not route a new method through
        // getIntArg without adding it to `intPos` first.
        return unwrapNarrowedNullableOperand(lowerExpr(*call.args[idx].value),
                                             call.args[idx].value.get());
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
            // per stdlib §2.3, v0.315: accepts exactly "true"/"false" (case-insensitive).
            // Anything else raises FormatException.
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
            llvm::Constant* falseStr = llvm::ConstantDataArray::getString(llvm_, "false", true);
            auto* falseGV = new llvm::GlobalVariable(*module_, falseStr->getType(), true,
                llvm::GlobalValue::PrivateLinkage, falseStr, ".bz.bool.false." + std::to_string(tid));
            llvm::Value* cmpFalse = builder_.CreateICmpEQ(
                builder_.CreateCall(strcmpFn, {data, falseGV}),
                llvm::ConstantInt::get(i32(), 0));
            // Neither "true" nor "false" → raise FormatException.
            llvm::Value* notTrueOrFalse = builder_.CreateNot(
                builder_.CreateOr(cmpTrue, cmpFalse), "tobool.invalid");
            emitGuardThrow(notTrueOrFalse, "FormatException");
            return cmpTrue;
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
        // ignoreCase=true → the code-point-wise case-folded compare
        // (bz_str_equals_ignorecase, fix #4483); false → plain equality.
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
        // #1244 / §9.6: null-guard for `other` in the case-insensitive branch.
        // A non-null `self` can never equal null, so the result is false
        // without dereferencing anything. (bz_str_equals_ignorecase below is
        // itself null-safe on both operands; the guard is kept so #1244's
        // contract — a null `other` is `false`, not a SIGSEGV — and this PHI's
        // shape stay exactly as tested.)
        auto* icNull_ = llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* otherIsNull = builder_.CreateICmpEQ(other, icNull_, "eq.ic.onull");
        auto* icNullBranch = builder_.GetInsertBlock();
        auto* icNonNullBB = llvm::BasicBlock::Create(llvm_, "eq.ic.cmp", currentFn_);
        builder_.CreateCondBr(otherIsNull, endBB, icNonNullBB);
        // fix #4483 (stdlib §9.6, v0.306): delegate the case-folded compare to
        // the runtime. What stood here was an inlined "fold each side into a
        // fresh buffer, then emitStringEq the buffers" loop carrying TWO
        // independent defects:
        //
        //   1. It called <ctype.h>'s tolower() on the result of loadCodePoint
        //      — the WIDTH-AWARE decoder (§4.6) — so any code point above
        //      U+00FF went outside tolower's domain, undefined per C11 §7.4p1.
        //      This is the last instance of the class #4420 removed from
        //      char.*/IsNullOrWhiteSpace; the three surviving ctype call sites
        //      in this backend are fed by CreateZExt from i8 and are in-domain.
        //
        //   2. It stored the folded code point `Trunc`'d to i8 at a hardcoded
        //      stride of 1 byte while the source index advanced one CODE POINT
        //      at a time, and labelled the result width=1 — so every code point
        //      above U+00FF became its low byte and distinct strings compared
        //      EQUAL ("š".Equals("a", true) was true: U+0161's low byte is
        //      0x61). Independent of the UB, and what actually made the wrong
        //      answer observable on glibc (whose out-of-line tolower happens to
        //      range-check).
        //
        // Folding per code point needs no buffer at all, so the fix is to
        // delete the materialisation rather than repair its width arithmetic —
        // which also drops the two undisposed allocations per operand the old
        // lowering leaked. bz_str_equals_ignorecase folds through
        // bz_char_to_lower, the same shared helper #4420 introduced, so there
        // is ONE case policy in the runtime rather than a second copy here
        // (#2590). It joins the width-aware bz_str_* family
        // (bz_str_startswith/_contains/_indexof) and takes the string objects
        // directly, so each operand is decoded at its own width.
        builder_.SetInsertPoint(icNonNullBB);
        auto* icFn = declareC("bz_str_equals_ignorecase", i32(), {ptrTy(), ptrTy()});
        llvm::Value* icEq = builder_.CreateTrunc(
            builder_.CreateCall(icFn, {self, other}, "eq.ic.res"), i1(), "eq.ic.bool");
        auto* icEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 3, "eq.result");
        phi->addIncoming(plainEq, planEnd);
        phi->addIncoming(builder_.getFalse(), icNullBranch);  // other==null → false
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

    // fix #3853 (stdlib §9.6, v0.290; proposal 0123 R6): CompareTo(other,
    // ignoreCase: bool): int — ordinal comparison, optionally ASCII
    // case-folded. `other` is non-nullable (checkBuiltinStringCallArgs'
    // #2315 gate covers this position for both CompareTo arities, so a
    // well-typed call's `other` is guaranteed non-null here — the same
    // premise the 1-arg arm above rests on for emitStringOrdCmp).
    //
    // fix #4487 (stdlib §9.6 + spec §4.6, v0.306): this passed
    // `loadStringData` (the RAW BUFFER) and `loadStringLen` (the CODE-POINT
    // count) to `bz_string_compare`, which then walked `min(a_len, b_len)`
    // BYTES — the #535 defect class. For a width-2 operand it read only the
    // first `len` of the `len*width` data bytes, so two DIFFERENT strings
    // compared EQUAL. Measured on HEAD (4d9d186a) before this fix:
    // "日本".CompareTo("日x", true) = 0, "š".CompareTo("a", true) = 0,
    // "š".CompareTo("ɡ", true) = 0. (No UB was involved — that helper's
    // `tolower` calls were on `(unsigned char)` values and in-domain — only
    // wrong values.) It now takes the string OBJECTS, so `bz_str_compare_cp`
    // decodes each at its own width and the `ignoreCase` fold goes through
    // `bz_char_to_lower`, the one shared case policy (#4420/#4483, #2590).
    // Both `CompareTo` arities and the §4.6 ordering operators are now the
    // same runtime relation, which is what those two documents require.
    if (method == "CompareTo" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* other = getStrArg(0);
        if (!other) return nullptr;
        llvm::Value* ic = lowerExpr(*call.args[1].value);
        if (!ic) return nullptr;
        llvm::Value* ic32 = ic->getType()->isIntegerTy(32)
            ? ic : builder_.CreateZExt(ic, i32());
        auto* fn = declareC("bz_str_compare_cp", i32(),
                            {ptrTy(), ptrTy(), i32()});
        return builder_.CreateCall(fn, {self, other, ic32}, "str.cmp.ci");
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
        // §9.6 v0.215 / proposal 0062 item 2: IndexOf(value: char, startIndex: int).
        // Linear scan starting at the code-point index startIndex; startIndex is
        // required to lie in [0, Length] (IndexOutOfRangeException otherwise).
        if (call.args.size() == 2 && call.args[0].value && call.args[1].value) {
            auto* argTy = ctx_.exprType(*call.args[0].value);
            if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
                argTy->primitive == sema::PrimitiveType::Char) {
                llvm::Value* needle = lowerExpr(*call.args[0].value);
                if (!needle) return nullptr;
                llvm::Value* startV = lowerExpr(*call.args[1].value);
                if (!startV) return nullptr;
                // Normalize startV to i64 for the loop counter / bounds check.
                llvm::Value* start64 = startV->getType()->isIntegerTy(64)
                    ? startV
                    : builder_.CreateSExt(startV, i64(), "ioc2.s64");
                llvm::Value* len = loadStringLen(self);
                // Bounds check: 0 <= startIndex <= Length.
                llvm::Value* oor = builder_.CreateOr(
                    builder_.CreateICmpSLT(start64, builder_.getInt64(0), "ioc2.sneg"),
                    builder_.CreateICmpSGT(start64, len, "ioc2.sbig"), "ioc2.oor");
                emitGuardThrow(oor, "IndexOutOfRangeException");
                // Inline forward scan from startIndex, mirroring the 1-arg form.
                auto* idxS = entryAlloca(i64());
                builder_.CreateStore(start64, idxS);
                auto* cBB = llvm::BasicBlock::Create(llvm_, "ioc2.c", currentFn_);
                auto* bBB = llvm::BasicBlock::Create(llvm_, "ioc2.b", currentFn_);
                auto* sBB = llvm::BasicBlock::Create(llvm_, "ioc2.s", currentFn_);
                auto* eBB = llvm::BasicBlock::Create(llvm_, "ioc2.e", currentFn_);
                builder_.CreateBr(cBB);
                builder_.SetInsertPoint(cBB);
                llvm::Value* idx = builder_.CreateLoad(i64(), idxS);
                builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bBB, sBB);
                builder_.SetInsertPoint(bBB);
                llvm::Value* idxB = builder_.CreateLoad(i64(), idxS);
                llvm::Value* cp = loadCodePoint(self, idxB);
                llvm::Value* found = builder_.CreateICmpEQ(cp, needle);
                auto* incBB = llvm::BasicBlock::Create(llvm_, "ioc2.inc", currentFn_);
                builder_.CreateCondBr(found, eBB, incBB);
                builder_.SetInsertPoint(incBB);
                builder_.CreateStore(builder_.CreateAdd(idxB, builder_.getInt64(1)), idxS);
                builder_.CreateBr(cBB);
                builder_.SetInsertPoint(sBB);  // not found
                builder_.CreateBr(eBB);
                builder_.SetInsertPoint(eBB);
                llvm::Value* fi = builder_.CreateLoad(i64(), idxS);
                // If we exited because idx >= len (sBB), result = -1.
                llvm::Value* exhausted = builder_.CreateICmpSGE(fi, len, "ioc2.exhausted");
                llvm::Value* result = builder_.CreateSelect(exhausted,
                    llvm::ConstantInt::getSigned(i32(), -1),
                    builder_.CreateTrunc(fi, i32()), "ioc2.result");
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

    // §9.6 / #1295: PadLeft(totalWidth[, padChar]) / PadRight(totalWidth[, padChar]).
    // Delegate to the width-aware runtime helper bz_str_pad. The prior inline
    // lowering assumed 1 byte per code point (allocated totalWidth+1 bytes,
    // memcpy'd the code-point COUNT as a byte count, and hard-stored width=1),
    // so any code point > U+00FF was corrupted. Strings store fixed-width code
    // points (§4.6); the runtime builds the padded result at the correct width
    // (widest of self's width and the pad char's), widening self's code points.
    if ((method == "PadLeft" || method == "PadRight") &&
        !call.args.empty() && call.args[0].value) {
        llvm::Value* widthArg = lowerExpr(*call.args[0].value);
        if (!widthArg) return nullptr;
        llvm::Value* width64 = builder_.CreateSExt(widthArg, i64(), "pad.width");
        // Pad character: default space, or the 2nd arg (a code point, i32).
        llvm::Value* padCharI32 = builder_.getInt32(' ');
        if (call.args.size() >= 2 && call.args[1].value) {
            llvm::Value* pc = lowerExpr(*call.args[1].value);
            if (pc && pc->getType()->isIntegerTy(32)) padCharI32 = pc;
            else if (pc) padCharI32 = builder_.CreateZExtOrTrunc(pc, i32());
        }
        // leftAlign = 1 for PadRight (self first, pad after); 0 for PadLeft.
        llvm::Value* leftAlign =
            builder_.getInt8(method == "PadRight" ? 1 : 0);
        auto* fn = declareC("bz_str_pad", ptrTy(),
                            {ptrTy(), i64(), i32(), llvm::Type::getInt8Ty(llvm_)});
        return builder_.CreateCall(fn, {self, width64, padCharI32, leftAlign},
                                   "pad.result");
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
    // fix #3423 (§22.5, v0.283): apply the binding permutation so reordered
    // named args are lowered correctly. Parameter p receives
    // call.args[paramToArg[p]] (not call.args[p] positionally).
    if (method == "Remove" && !call.args.empty() && call.args[0].value) {
        const auto* bnd = ctx_.builtinCallBinding(call);
        std::size_t startArgIdx = 0;
        std::size_t countArgIdx = 1;
        if (bnd && bnd->size() == static_cast<std::size_t>(call.args.size())) {
            // 1-arg form: only param 0 (start)
            if (call.args.size() == 1) {
                startArgIdx = (*bnd)[0];
            }
            // 2-arg form: param 0 (start), param 1 (count)
            if (call.args.size() == 2) {
                startArgIdx = (*bnd)[0];
                countArgIdx = (*bnd)[1];
            }
        }
        llvm::Value* startV = getIntArg(startArgIdx);
        if (!startV) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* selfLen  = loadStringLen(self);
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* start64 = builder_.CreateSExt(startV, i64(), "rm.start");
        // end = start + count (if given), else = selfLen
        llvm::Value* end64;
        if (call.args.size() >= 2 && call.args[1].value) {
            llvm::Value* cntV = lowerExpr(*call.args[countArgIdx].value);
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
    // fix #3615 (§9.1 / §9.2 / §19.3 / §2.8.2 / §2.8.3 rule 4, v0.286): DELEGATE to the ONE
    // both-sources storage-component accessor, `sema::storageComponentType`
    // (declared sema/Symbol.hpp, body SymbolCollection.cpp) — do not spell the
    // three-source lookup a second time here.
    //
    // This function used to be that second spelling, and it had drifted in the
    // one way that matters: its imported fallback was gated on
    // `if (!fs->posParam && !fs->decl)`. Fix #2874's own comment on that gate
    // NAMED the case the gate excludes — "the loader synthesizes a null-type
    // posParam only for §9.1 positional-vs-body discrimination, not the type
    // itself; the type itself is never recoverable from posParam for a #2516
    // imported positional field". A #2516 imported positional field therefore
    // has a NON-NULL synthetic `posParam` (ModuleLoader.cpp `if (mm.isPositional)`),
    // so `!fs->posParam` is false and the fallback #2874 added never ran for
    // exactly the field kind its comment describes. `storageComponentType`
    // reaches `ctx.importedMemberType(m)` unconditionally, which is what the
    // loader's own #2516 comment says the design relies on.
    //
    // MEASURED — the two live observables this closes are §19.3 record
    // destructuring sub-patterns, whose lowering (LlvmBackend_Exprs2.cpp) asks
    // `recordMemberType` for the slot's declared type. Over a `record
    // Tag(Name: string, C: Color)` exported by a dependency, each against a
    // character-identical single-module twin: a `string` sub-pattern compared the
    // slot by POINTER instead of by content (a runtime-built "abc" took the `_`
    // arm cross-module and the matching arm locally — exit 0, wrong value), and
    // an enum-constant sub-pattern was a hard backend reject ("the `case`/`when`
    // pattern `Blue` is not supported by the backend ... (§19.3)").
    //
    // ⚠ WHAT THIS DOES NOT CLOSE, measured on BOTH sides of this change: an
    // imported record's `string` component produced by an imported factory still
    // LEAKS at the consumer's scope end (8290 B in 400 allocations at 200
    // iterations; the single-module twin is clean). `isStringStorageMember`
    // (LlvmBackend_impl.hpp) does read this function, but that release is gated
    // earlier by the `recordCallResultReleaseEligible_` pass, so a null field
    // type was not what suppressed it. Filed separately with the repro; do not
    // read this delegation as having fixed it.
    //
    // Related, and worth correcting where it stands: fix 2874's comment asserted
    // that the store-side retain and the disposal-side release are "both skipped,
    // so the string is balanced by accident". That does not hold for the
    // cross-module shape — the `+1` is minted in the DEPENDENCY and only the
    // consumer ever sees the value die, so skipping the consumer side leaks
    // rather than balances. Its warning that flipping the predicate in only ONE
    // place turns a leak into a UAF still stands for the shapes where both sides
    // are in the same module, and this edit does flip both together.
    return sema::storageComponentType(fs, ctx_);
}

const sema::TypeRef* Emitter::recordMemberType(const sema::Symbol* m) {
    // fix #3615: same delegation. The old body's §9.2 property arm read the type
    // off `dynamic_cast<const ast::PropertyDecl*>(m->decl)` with NO imported
    // fallback at all, so every imported auto-property answered null here.
    // `storageComponentType`'s PropertySymbol arm has the same two AST steps
    // plus `ctx.importedMemberType(m)`. The dispatch is equivalent on the local
    // side: SymbolCollection.cpp:236 makes a `PropertySymbol` for every
    // `ast::PropertyDecl` and nothing else is ever given one, so
    // "decl is a PropertyDecl" and "symbol is a PropertySymbol with an AST
    // decl" name the same set.
    if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m))
        return recordFieldType(fs);
    return sema::storageComponentType(m, ctx_);
}

// stdlib §3.1 / §8.5: `StringBuilder.ToString()` — concatenate the parts array
// into ONE freshly malloc'd `%bz.string` with rc := 1 (a `+1` the consumer
// discharges once). Factored out of the collection-call lowering by #2923 so
// the direct call `sb.ToString()` and the §18.5 interpolation hole `$"{sb}"`
// share one lowering instead of two hand-maintained spellings of stdlib §3.1's
// declared member. // per stdlib §3.1 / §8.5 / §18.5
llvm::Value* Emitter::emitStringBuilderToString(llvm::Value* sb) {
    if (!sb || !sb->getType()->isPointerTy()) return nullptr;
    // Use the runtime helper to concatenate all string parts.
    auto* outDataSlot = entryAlloca(ptrTy());
    auto* outLenSlot  = entryAlloca(i64());
    auto* fn = declareC("bz_strbuilder_to_string",
                        llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy(), ptrTy()});
    builder_.CreateCall(fn, {sb, outDataSlot, outLenSlot});
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


// fix #2923 (spec §8.5 / §8.1 / §18.5 / §28.9, v0.240): render a VTABLE-LESS
// BUILT-IN CLASS receiver, i.e. one whose runtime value is a plain runtime C
// struct (`bz_array_t*`, `bz_mutex_t*`, `bz_random_t*`, `bz_event_source_t*`,
// …) with no Bezel object header. Returns null when `rt` is not such a type, so
// callers can chain it ahead of their existing fallback.
//
// WHY THIS EXISTS. `lowerObjectToString`'s contract is that the dynamic value
// is either a `%bz.string` or a vtable-bearing object; it separates the two
// with `bz_obj_is_string`, which is a NEGATIVE test — "word 0 is not one of the
// emitted vtables ⇒ it is a string" (bezel_runtime_refcounting.cpp). A built-in
// class satisfies neither half of the premise: it has no vtable, so the test
// answers "string", and the receiver is then read as a `%bz.string` (garbage
// data/len/width) and — since #2846 made that arm RETAIN what it returns —
// retained through the receiver's own bytes at the rc offset and RELEASED by
// the consumer. MEASURED on f069645: `using var x := new Mutex();
// Console.WriteLine($"val={x}")` printed `val=` and aborted with
// "free(): double free detected in tcache 2" (exit 134); the same receiver
// twice in one interpolation escalated to SIGSEGV; `new Random()` under ASan
// gave a heap-buffer-overflow READ. Every built-in class receiver tested
// reproduced some form of it — including `Array<int>` and `Dictionary<K,V>`,
// which #2923's own scoping had classified as unaffected.
//
// WHAT IT RENDERS. §8.5's class bullet is unconditional — "For classes: the
// type's qualified name" — so that is the answer for every one of them except
// the two whose stdlib declaration DOES include a `ToString(): string` of its
// own (`StringBuilder`, stdlib §3.1; `JsonSerializer`, stdlib §2.15 — the only
// two among the pointer-shaped reserved built-in classes, checked against the
// stdlib document). Those dispatch their own member instead, exactly as §18.5's
// "A hole *without* a format spec formats via the value's ordinary
// `ToString()`" requires. The type-name literal is a constant global with a
// SENTINEL rc, so the caller's ordinary release of a rendered hole is a no-op
// on it — the same discharge shape the user-class-without-override default
// (`lowerStringLiteral(ts->name)`, just above `emitValueToString`'s tail)
// already relies on.
llvm::Value* Emitter::emitVtablelessBuiltinToString(llvm::Value* val,
                                                    const sema::TypeRef* rt) {
    if (!val || !val->getType()->isPointerTy()) return nullptr;
    std::string_view bn = builtinTypeName(rt);
    if (bn.empty() || !common::isVtableLessBuiltinClassName(bn)) return nullptr;
    // stdlib §3.1: `StringBuilder` declares `public method ToString(): string`,
    // so §8.5's type-name default does NOT apply to it — it renders its content.
    if (bn == "StringBuilder") return emitStringBuilderToString(val);
    // stdlib §2.15: `JsonSerializer` declares `public method ToString(): string`
    // ("convenience: UTF-8 → string"), lowered to bz_jser_to_string by the §2.15
    // instance-method handler (LlvmBackend_Serialization.cpp).
    if (bn == "JsonSerializer")
        return builder_.CreateCall(
            declareC("bz_jser_to_string", ptrTy(), {ptrTy()}), {val},
            "jser.str");
    // §8.5: "For classes: the type's qualified name" — pinned by spec §8.5
    // to stdlib §2.13's `Type.FullName` rendering ("The default and `FullName`
    // are one rendering contract, not two", #2934). fix #3119: this site is
    // the ONE §8.5 emission that has the FULL `TypeRef` in hand — pass it
    // through `spec85TypeRefQualifiedName`, which resolves (a) the module
    // path of a reserved built-in class via `common::BuiltinRegistry` (so
    // `Mutex` becomes "System.Threading.Mutex"), and (b) stdlib §2.13 rule
    // 3's generic type-arg rendering (so `Array<int>` becomes
    // "System.Collections.Generic.Array<int>"). A builtin name the
    // module-path table does not know (`bn` is the simple head) renders
    // through the head lookup with no `<args>` — the table lookup is the
    // only place the module path lives, so a missing entry surfaces as the
    // bare head, not a wrong rendering.
    std::string_view mod = common::builtinClassModulePathOf(bn);
    std::string rendered;
    if (rt && !rt->genericArgs.empty()) {
        // Generic instantiation: the FULL §2.13 rule 3 rendering, recursing
        // on the type arguments. `bn` is the head; the table supplies the
        // module path; the generic args come from the TypeRef.
        rendered = std::string(mod.empty() ? "" : mod);
        if (!rendered.empty()) rendered += ".";
        rendered += std::string(bn);
        rendered += "<";
        for (std::size_t i = 0; i < rt->genericArgs.size(); ++i) {
            if (i) rendered += ", ";
            rendered += spec85TypeRefQualifiedName(rt->genericArgs[i]);
        }
        rendered += ">";
    } else if (!mod.empty()) {
        rendered = std::string(mod) + "." + std::string(bn);
    } else {
        rendered = std::string(bn);
    }
    return lowerStringLiteral(std::string("\"") + rendered + "\"", false);
}


// fix #2969 (spec §8.5 / §8.1 / §18.5 / §28.9, v0.240): a special case of
// `emitVtablelessBuiltinToString` — the static type `frt` is a BUILT-IN
// INTERFACE HEAD (IEnumerable<T>, IList<T>, IDictionary<K,V>, …), but the AST
// source `src` narrows the runtime value to a vtable-less built-in class.
//
// WHY THIS EXISTS (and is not just a call into emitVtablelessBuiltinToString).
// #2923's fix gates on the STATIC type being a vtable-less built-in CLASS —
// it deliberately excluded interface heads from the discriminator, because an
// interface slot may LEGITIMATELY hold a user class declaring that interface
// (`class MyE: IEnumerable<int>`, `var e: IEnumerable<int> := new MyE();
// e.Dispose();` compiles and runs on 09ed9ca). Routing an interface slot
// through `emitVtablelessBuiltinToString` statically would (a) render
// "IEnumerable" instead of "MyE" for a user class — wrong per §8.5 — and (b)
// stop disposing user-class instances through vtable[kDisposeSlot], which
// #2975 has only just made safe.
//
// But for the COMMON shaping of an interface-head-typed local — the
// initializer is a known constructor of a built-in collection, e.g.
// `var e: IList<int> := new Array<int>(2);` (a direct NewExpr) or
// `var e: IList<int> := a;` where `a: Array<int>` (an IdentifierExpr whose
// sema type is the built-in class) — the #2923 premise DOES hold on the
// runtime value. The renderer just needs the runtime TYPE, which the
// initializer's static type already names.
//
// HOW THE SOURCE-TYPE LOOKUP WORKS. `src` is the interpolation hole's AST
// expression. For the only shapes this fix admits:
//   * `IdentifierExpr` — a plain local reference. Its sema symbol is a
//     `LocalSymbol` whose `decl` is the `LocalDecl`; we read the decl's
//     initializer's sema type (`ctx_.exprType(*ld->initializer)`). That type
//     is the source of truth — for `var e: IList<int> := a;` where `a:
//     Array<int>`, it returns `Array<int>` even though `ld->type` declares
//     `IList<int>`.
//   * A NON-`IdentifierExpr` shape (a member access on an object, a method
//     call returning the interface head, a generic instantiation through a
//     type parameter) returns nullptr — the #2923 dynamic path remains in
//     force, which is correct for the user-class arm and DEFERS the
//     unanalysed built-in case to a future design decision per #2969.
//
// SAFETY / WHAT IT DOES NOT DO. It does not modify `lowerObjectToString`, so
// the user-class arm keeps its correct vtable dispatch. It does not change
// `bz_obj_is_string`, so the interface-head -> vtable path remains available.
// The receiver retains NOTHING — the returned string is a `lowerStringLiteral`
// constant with a SENTINEL rc, the same discharge shape #2923 already relies
// on. And the #2975 disposal fix (interface slot holding a raw user-class
// instance routes to the class disposal) is untouched — its predicate is
// `iterFormatWrappedDecls_`, never reaches the BuiltinInterfaceHead path here.
//
// WHAT IT DOES NOT FIX. The explicit `.ToString()` call on a built-in
// interface head (rejected at sema on the parent commit, #2457). Logged in
// the test's header so the boundary is explicit.
//
// fix #2968 (spec §8.1 / §8.5 / §18.5 / §28.9, v0.240): the OBJECT-slot
// spelling — which #2969 recorded above as its own deferred residual — is the
// SAME defect one level further out, and it admits the SAME remedy, so this
// helper now gates on `Object` as well as on the interface heads.
//
// §8.1 makes `var o: Object := mx;` legal without a conversion, verbatim:
//
//   "classes, interfaces, `string`, heap arrays (`Array<T>`,
//    `OwningArray<T>`), and function types are reference types whose values
//    are already heap-allocated and reach `Object` directly through the
//    inheritance chain"
//
// So the built-in sits in the slot AS ITSELF, with no vtable at word 0, and
// `bz_obj_is_string`'s NEGATIVE test ("word 0 is not one of the emitted
// vtables ⇒ a string") answers "string" for it. `lowerObjectToString` then
// returns the receiver as a string and retains it — the retain landing on the
// built-in's own bytes at the `%bz.string` rc offset — and the consumer's
// matching release frees a live object. That is #2968's measured double free.
//
// WHY BOXING IS NOT THE FIX. #2968's filing offered "box the built-in on
// assignment to `Object`" as option 2. The §8.1 sentence quoted above puts
// these receivers explicitly on the REFERENCE side of §8.6's boxing line, so
// boxing them would contradict §8.1's classification. Option 2 is closed
// unless the spec is amended, which is a separate workflow.
//
// WHY THE `Object` SLOT IS SAFE TO ADMIT HERE (and the string hazard the
// interface-head arm never faced). An interface-head slot cannot hold a
// `string`; an `Object` slot can. The narrowing below is therefore load-
// bearing in a way it was not for #2969: it fires ONLY when the initializer's
// static type is a vtable-less built-in CLASS. `string` is in neither reserved
// table (common/BuiltinRegistry.cpp), so `isVtableLess` answers false for it
// and a genuine `var o: Object := "hi";` still falls through to
// `lowerObjectToString`, whose `bz_obj_is_string` premise is TRUE for a real
// string. A user class likewise carries a `declaration` and falls through to
// its correct vtable dispatch. Only the receivers whose premise is false are
// routed away.
llvm::Value* Emitter::tryOpaqueSlotBuiltinRender(llvm::Value* val,
                                                 const sema::TypeRef* frt,
                                                 const ast::Expr* src) {
    if (!val || !val->getType()->isPointerTy()) return nullptr;
    if (!frt) return nullptr;
    // The two OPAQUE slot spellings that reach this helper. Note they are
    // different TypeRef KINDS, which is why the gate is not a single test:
    //   * `Object` — sema models the universal root as a PRIMITIVE
    //     (TypeResolution.cpp maps the name to `PrimitiveType::Object`), NOT
    //     as a Named ref. This is #2968's slot, and it is also why the #2969
    //     Named-only gate never fired for it.
    //   * a built-in INTERFACE HEAD — a declaration-less Named ref (#2969).
    const bool isObjectSlot =
        frt->kind == sema::TypeRef::Kind::Primitive &&
        frt->primitive == sema::PrimitiveType::Object;
    if (!isObjectSlot) {
        // #2923 deliberately excluded interface heads from the vtable-less
        // discriminator; the interface slot may hold a user class. We are the
        // OTHER arm of the exclusion — we only fire when the AST source
        // statically narrows the runtime value to a built-in class.
        if (frt->kind != sema::TypeRef::Kind::Named || frt->declaration)
            return nullptr;
        std::string_view bn = builtinTypeName(frt);
        if (bn.empty() || !common::isBuiltinInterfaceHead(bn)) return nullptr;
    }
    if (!src) return nullptr;
    // #2923's `common::isVtableLessBuiltinClassName` test asks whether the
    // RUNTIME TYPE carries no Bezel vtable at word 0 — i.e. is one of the
    // built-in classes whose runtime value is a plain C struct
    // (`bz_array_t*`, `bz_dict_t*`, `bz_mutex_t*`, …). Same predicate, same
    // table, same answer.
    auto isVtableLess = [this](const sema::TypeRef* rt) -> bool {
        if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
        std::string_view n = builtinTypeName(rt);
        if (n.empty()) return false;
        return common::isVtableLessBuiltinClassName(n);
    };
    // Only the IdentifierExpr shape is admitted here — the #2975 prior art
    // shows the precise AST-and-sema walk this fix mirrors (the #2975
    // `rawUserClassInBuiltinInterfaceSlot` reads the same `ld->initializer`
    // source type for the disposal-side decision). A method-call or
    // member-access source would need a wider analysis and is left for a
    // future lane once the owner-signed-off direction for #2969 lands.
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(src);
    if (!id) return nullptr;
    sema::Symbol* sym = ctx_.exprSymbol(*id);
    auto* loc = dynamic_cast<sema::LocalSymbol*>(sym);
    if (!loc || !loc->decl) return nullptr;
    auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);
    if (!ld || !ld->initializer) return nullptr;
    // fix #2968: the narrowing below reads the INITIALIZER's static type, so
    // it describes the slot only while the slot still holds the initializer's
    // value. A statement-level reassignment invalidates it — and for an
    // `Object` slot the wrong answer would be a REGRESSION, not merely a
    // missed fix (`var o: Object := mx; o := "hi";` renders correctly TODAY
    // via the dynamic path, whose `bz_obj_is_string` premise a real string
    // satisfies). Decline rather than narrow.
    if (reassignedOpaqueLocals_.count(ld)) return nullptr;
    const sema::TypeRef* srcRt = ctx_.exprType(*ld->initializer);
    if (!isVtableLess(srcRt)) return nullptr;
    // fix #3127 (§8.6 / §8.5): `Range<T>` IS in `kNonHeapReservedBuiltins`
    // (so `isVtableLessBuiltinClassName` answers true), but #3127 gave it a
    // real Bezel vtable (Range is now boxable per §8.6, dispatch through
    // vtable[kToStringSlot]). The #2968 #2923 helper bypasses the dispatch —
    // it would return the literal type name "Range" — exactly the wrong
    // rendering #3127 exists to fix. Decline the helper for any Range
    // initializer so the interp falls through to lowerObjectToString →
    // vtable[kToStringSlot] → emitRangeToString.
    if (rangeBoxable(srcRt)) return nullptr;
    // The receiver retains NOTHING (the literal has a sentinel rc), so this
    // is a pure replacement of the broken `lowerObjectToString` call site —
    // no memory obligation added for the renderer.
    return emitVtablelessBuiltinToString(val, srcRt);
}


// Format a single field/element value of static type `fieldTy` as a string,
// recursing for nested records (§9.1) and using each type's own ToString.
llvm::Value* Emitter::emitValueToString(llvm::Value* val,
                                        const sema::TypeRef* fieldTy) {
    using K = sema::TypeRef::Kind;
    // fix #3442 (§8.5 *`ToString`* / §4.7 / §18.5, v0.283): the UNIT row of
    // §8.5 — "and for the unit type: the type's name — … `"()"` for unit". This
    // function is the shared value-rendering authority every non-primitive
    // §18.5 hole and every record component bottoms out in (the InlineArray arm
    // below delegates here for exactly that reason, #2913), and it had NO unit
    // arm at all: a unit-typed operand fell past every arm to the trailing
    // LLVM-shape fallback, where the unit slot's one INERT placeholder byte
    // (`unitPlaceholderTy`) matched `isIntegerTy()` and rendered as `"0"` —
    // MEASURED: `$"[{u}]"` on `var u: () := ()` printed `[0]`, and `$"[{p}]"`
    // on a PRESENT `()?` printed `[0]` too, while `$"[{u.ToString()}]"` on the
    // very same value printed `[()]`. Two spellings of one §8.5 rule
    // disagreeing inside one program, and `0` is a rendering the document
    // permits for no unit-typed expression. The byte is the one whose "CONTENTS
    // are never meaningfully read" (#2915 / #3421 / isNullableUnitTy) — reading
    // it was the bug.
    //
    // Placed FIRST, ahead of every `val->getType()` dereference, because `val`
    // is legitimately NULL here: `()` lowers to LLVM `void`, so `lowerExpr`
    // returns nullptr for every unit-typed expression by construction. §4.7
    // gives the unit type exactly one value, so §8.5's answer does not depend
    // on `val` at all and nothing is lost by not having one. The `()?` sibling
    // needs no arm of its own — the nullable-VALUE arm just below already
    // branches on the tag and recurses with the non-nullable inner type, which
    // lands exactly here, so a present `()?` renders `"()"` and a null one
    // keeps §18.5's `"null"`.
    if (fieldTy && fieldTy->kind == K::Unit && !fieldTy->nullable)
        return lowerStringLiteral(kUnitToStringSource, false);
    // fix #5943 (§27.7.1 / §8.5, v0.338): a function value renders as its
    // `ToString` — the formed type — at every rendering site, through the
    // cell's object view (a no-op on a value already converted).
    if (fieldTy && fieldTy->kind == K::Function && !fieldTy->nullable &&
        val && val->getType()->isPointerTy())
        return lowerObjectToString(fnValToObjectView(val));
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
    // fix #2913 (spec §8.5 inline-array row / §18.5, v0.240): an INLINE ARRAY
    // renders as its type's NAME. §8.5 is explicit, and explicit about why:
    //
    //   "For inline arrays (all forms, including move-only `[N owned]…` arrays,
    //    whose receiver is borrowed for the call) and for the unit type: the
    //    type's name — `"[3]int"`-style for an inline array … (element-listing
    //    output is deliberately not synthesized — an inline array's elements may
    //    be class-typed borrows whose formatting is not the container's
    //    business)."
    //
    // #2804 gave the BOXED path this rendering, through the same
    // `inlineArrayTypeSpelling` called here, so `o.ToString()` on an
    // `Object`-typed inline array already answered `"[3]int"`. The UNBOXED
    // interpolation path had no arm at all and fell through to the trailing
    // empty-string fallback, so `$"{a}"` rendered the EMPTY STRING with no
    // diagnostic — a silent wrong value, and one that disagreed with the boxed
    // spelling of the very same value in the very same program.
    //
    // Deliberately the SAME function the box thunk uses rather than a second
    // renderer: two spellings of one rule must not be able to drift, which is
    // this run's recurring lesson. It is also why no `arrayOwned` special case
    // is needed — §8.5 puts move-only arrays in the same row, and the spelling
    // function already emits the `owned` marker.
    if (fieldTy && fieldTy->kind == K::InlineArray) {
        return lowerStringLiteral(
            "\"" + inlineArrayTypeSpelling(fieldTy) + "\"", false);
    }
    if (fieldTy && fieldTy->kind == K::Primitive) {
        // §9.1: strings are formatted by their own ToString — which for
        // string is identity (§9.6); the value is the bz_string itself.
        return emitPrimitiveToString(val, fieldTy->primitive);
    }
    // §2.18 / §18.5 (#1411): named builtin value types with their own no-format
    // renderers. TimeSpan → "c" (§2.5), DateOnly → "yyyy-MM-dd" ISO 8601,
    // TimeOnly → bz_timeonly_to_string, Guid → "D" (§2.6 / §2.18). Without
    // this, they fell through the Class dispatch below (integer LLVM types
    // don't match the isPointerTy guard; a Guid struct has no members) and
    // rendered raw tick/day counts or the literal type name.
    if (fieldTy && fieldTy->kind == K::Named) {
        std::string_view bn = builtinTypeName(fieldTy);
        if (bn == "TimeSpan" && val->getType()->isIntegerTy(64)) {
            auto* fn = declareC("bz_timespan_to_string_c", ptrTy(), {i64()});
            return builder_.CreateCall(fn, {val}, "ts.str");
        }
        if (bn == "DateOnly" && val->getType()->isIntegerTy()) {
            llvm::Value* dn = val->getType()->isIntegerTy(32)
                ? val : builder_.CreateTrunc(val, i32());
            llvm::Value* ticks = builder_.CreateMul(
                builder_.CreateSExt(dn, i64()),
                builder_.getInt64(864000000000LL));  // TICKS_PER_DAY
            llvm::Value* fmtStr = lowerStringLiteral("\"yyyy-MM-dd\"", false);
            auto* fn = declareC("bz_datetime_to_string_fmt", ptrTy(),
                                {i64(), ptrTy()});
            return builder_.CreateCall(fn, {ticks, fmtStr}, "do.str");
        }
        if (bn == "TimeOnly" && val->getType()->isIntegerTy()) {
            llvm::Value* ms = val->getType()->isIntegerTy(32)
                ? val : builder_.CreateTrunc(val, i32());
            auto* fn = declareC("bz_timeonly_to_string", ptrTy(), {i32()});
            return builder_.CreateCall(fn, {ms}, "to.str");
        }
        if (bn == "Guid") {
            llvm::StructType* gst = guidStructType();
            llvm::AllocaInst* slot = entryAlloca(gst);
            llvm::Value* gv = val;
            if (gv->getType()->isPointerTy())
                gv = builder_.CreateLoad(gst, gv, "guid.str.load");
            builder_.CreateStore(gv, slot);
            auto* fn = declareC("bz_guid_to_string", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {slot}, "guid.strD");
        }
    }
    // fix #2661 (spec §8.5, v0.252): the §8.5 defaults are UNIVERSAL — "generated
    // for **every** type of the applicable kind, wherever declared — user
    // modules and the standard library alike". The two stdlib value types whose
    // TypeRef carries no user `TypeSymbol` and whose value is a struct are the
    // span family (§4.14/§4.15) and `Range<T>` (§9.8); before this, both fell
    // past every arm below (a struct matches neither the pointer nor the
    // integer/float tests) to the trailing empty-string fallback, so `$"{sp}"`
    // rendered "" and `"x" + r.ToString()` silently dropped the statement.
    // ⚠ `ReadOnlyStringSpan` (§3.3) is EXCLUDED and must stay excluded — it
    // DECLARES its own materializing `ToString`, which `isSpanLikeTypeName`
    // does not match.
    if (fieldTy && isSpanLikeTypeName(fieldTy) && val->getType()->isStructTy())
        return emitSpanToString(val, fieldTy->headName());
    if (fieldTy && val->getType()->isStructTy() &&
        (isRangeType(fieldTy) ||
         (fieldTy->kind == K::Named && !fieldTy->declaration &&
          fieldTy->astNode && fieldTy->astNode->name &&
          !fieldTy->astNode->name->parts.empty() &&
          fieldTy->astNode->name->parts[0] == "Range")))
        return emitRangeToString(
            val, fieldTy->genericArgs.empty() ? nullptr : fieldTy->genericArgs[0]);
    if (fieldTy && fieldTy->kind == K::Named && fieldTy->declaration) {
        const sema::TypeSymbol* ts = fieldTy->declaration;
        if (ts->kind == ast::TypeDeclKind::Record && val->getType()->isStructTy()) {
            // fix #3496 (spec §8.5 / §18.5 / §9.1, v0.287): honor a user-declared
            // `ToString(): string` on the record BEFORE deciding the §8.5 default.
            // §8.5's first sentence pins the explicit `.ToString()` call and the
            // §18.5 hole to ONE rule: a record's declared member wins over the
            // synthesized field-listing default. Without this lookup
            // `emitValueToString` only knew the field-listing and the qualified
            // type-name defaults, so every path that recursed through here — the
            // §18.5 nullable-value-type arm in `LlvmBackend_Exprs.cpp` being the
            // headline one — silently DROPPED the override and rendered the
            // auto-property listing instead, agreeing with neither the direct
            // `.ToString()` call (which routes through `lowerCall` and finds the
            // user member) nor the non-nullable §18.5 hole (which uses the
            // interpolation-chain lookup at `LlvmBackend_Exprs.cpp:815`). The
            // interpolation-chain walk is mirrored here so the ONE renderer every
            // sibling spelling reaches has identical behaviour to all of them.
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
                // Spill the record value to an alloca; `emitMethodCall` expects
                // a `ptr` receiver for the value-struct shape.
                llvm::AllocaInst* spill = entryAlloca(val->getType());
                builder_.CreateStore(val, spill);
                std::vector<llvm::Value*> args{spill};
                if (llvm::Value* r = emitMethodCall(toStr, args, false))
                    return r;
            }
            // fix #2934 (§8.5, v0.252): ask the SAME three-way question the
            // direct `.ToString()` call asks (LlvmBackend_Calls2.cpp's
            // `autoBearing`) and the interpolation chain asks
            // (LlvmBackend_Exprs.cpp) — a body record that does NOT satisfy
            // §9.2's auto-equality conditions has NO field listing at all; its
            // §8.5 default is the qualified type name. This arm used to recurse
            // into `emitRecordToString` for EVERY record kind, so one program
            // could render the same ineligible record two ways: `Reports.Manual`
            // from `m.ToString()` / `$"{m}"`, and `Manual(A: 3)` from the very
            // same value reached as a record COMPONENT or through an alignment
            // spec (`$"{m,10}"`, emitWithFormatSpec's Named arm). §8.5's
            // component rule — "Each component renders as its own `ToString`
            // renders it" — makes those the same question, so they get the same
            // answer here.
            if (spec85NameOnlyDefaultIsQualified(ts))
                return lowerStringLiteral(
                    std::string("\"") + spec85QualifiedTypeName(ts) + "\"",
                    false);
            // fix #4234 (§9.1, v0.298): route the NESTED (field-level)
            // recursion through a per-INSTANTIATION MEMOIZED helper (a real
            // `call`) instead of re-inlining `emitRecordToString`'s body —
            // see `recordToStringHelperFn`'s header comment
            // (LlvmBackend_Setup.cpp) for the exponential-blowup this fixes
            // AND for why the key/load type must be `val`'s OWN already-
            // correct struct type, not one re-derived from `ts` alone.
            // Spill `val` so the helper can take it by pointer (the same
            // spill idiom the user-override arm above already uses).
            //
            // fix #4251 (§9.1/§14.1/§8.5, v0.298): install THIS field's own
            // generic substitution before the memoized helper's body is
            // (first) built — the ToString sibling of the identical
            // Equals/GetHashCode gap (see emitValueEquality's nested-record
            // arm, LlvmBackend_Layout.cpp, for the full mechanism). `fieldTy`
            // already carries the CONCRETE `genericArgs` for this field
            // (e.g. `Box<string>`); a no-op for a non-generic nested record.
            RecordSubstGuard fieldSubst(this, fieldTy);
            auto* recTy = llvm::cast<llvm::StructType>(val->getType());
            llvm::Function* helper = recordToStringHelperFn(ts, recTy);
            llvm::AllocaInst* spill = entryAlloca(recTy);
            builder_.CreateStore(val, spill);
            return builder_.CreateCall(helper->getFunctionType(), helper,
                                       {spill});
        }
        if (ts->kind == ast::TypeDeclKind::Enum)
            // per §10.5 (v0.183): a user `ToString(): string` replaces the §8.5
            // default (the type's qualified name) inside interpolation; `val`
            // is the enum's underlying integer.
            return emitEnumToString(val, ts);
        // #2479 / §35.7: a choice-typed field/interpolation-hole formats by
        // the §35.7 discriminant+payload format when the choice is auto-
        // hashable — mirrors the record recursion just above
        // (`emitRecordToString`'s reachability precondition already
        // guarantees any Choice-typed FIELD it recurses into is itself
        // auto-hashable, per §9.2/§35.7's recursive admissible-
        // component rule: an ineligible body record's ToString never
        // iterates fields at all, LlvmBackend_Calls2.cpp's `!autoBearing`
        // early return). A bare top-level choice-typed interpolation hole
        // (`$"{c}"`) carries no such precondition, so the check is real
        // here, not defensive: for a non-auto-hashable choice this
        // falls through to the generic fallback below (a struct value hits
        // neither the pointer nor the integer/float checks, landing on the
        // final `lowerStringLiteral("\"\"", false)` — an empty string,
        // not a crash; `lowerValueSemMethodOn`'s Choice branch (LlvmBackend
        // _Calls2.cpp) is the primary place this classification is
        // surfaced to the user via a real diagnostic, since a direct
        // `.ToString()` call is by far the common spelling).
        if (ts->kind == ast::TypeDeclKind::Choice &&
            val->getType()->isStructTy() &&
            sema::isAutoEqualityBearingChoice(ts, ctx_, fieldTy)) {
            // fix #4278: pass fieldTy (the concrete instantiation) so that
            // emitChoiceToString can use substitutedChoicePayloadType to resolve
            // the generic payload type T -> concrete type (e.g. string).
            RecordSubstGuard fieldSubst(this, fieldTy);
            return emitChoiceToString(val, ts, fieldTy);
        }
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
            // fix #2934 (§8.5, v0.252): the class default is the module-path-
            // qualified name (stdlib §2.13 `FullName`). This is the arm §8.5's
            // component rule names — "Each component renders as its own
            // `ToString` renders it (virtual dispatch — a class-typed component
            // with no override renders its class default, the qualified name
            // per the bullet above)" — so a record's class-typed FIELD renders
            // QUALIFIED inside the record's own UNQUALIFIED-headed rendering.
            // Both halves of that asymmetry are §8.5's, deliberately.
            return lowerStringLiteral(
                std::string("\"") + spec85QualifiedTypeName(ts) + "\"", false);
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
    // §8.5 (#1118): a generic type-parameter operand `$"{x}"` where `x: T` is an
    // unresolved Named TypeRef (no declaration). Resolve T to its concrete
    // instantiation via the active generic substitution and recurse, so a T
    // bound to a class/record/enum formats by that type's rule — in particular a
    // class with a ToString override dispatches virtually. Without this the value
    // (a class pointer, say) fell to the bz_string fallback below and rendered as
    // "" (the W pointer read as a string). The `!= fieldTy` guard avoids a
    // self-recursion if the substitution maps the name to itself.
    if (fieldTy && fieldTy->kind == K::Named && !fieldTy->declaration &&
        fieldTy->astNode && fieldTy->astNode->name &&
        fieldTy->astNode->name->parts.size() == 1 &&
        !genericSubstSemaTy_.empty()) {
        auto sit = genericSubstSemaTy_.find(fieldTy->headName());
        if (sit != genericSubstSemaTy_.end() && sit->second &&
            sit->second != fieldTy)
            return emitValueToString(val, sit->second);
    }
    // fix #2923 (spec §8.5 / §8.1 / §18.5 / §28.9, v0.240): a declaration-less
    // Named ref is one of exactly two things — a RESERVED BUILT-IN or a generic
    // type PARAMETER (common/BuiltinRegistry.hpp, `isReservedBuiltinTypeName`).
    // The #1118 fallback directly below is written for the second ("an
    // unresolved generic-T operand"), and its premise is FALSE for the first: a
    // built-in class is not a heap object with a vtable, so handing it to
    // `lowerObjectToString` makes `bz_obj_is_string` mis-answer "string" and the
    // receiver is retained + released as one — the #2923 double free. Ask the
    // built-in question FIRST; a generic-T operand is unaffected (its head is a
    // type-parameter name, which is in no builtin table).
    if (fieldTy && fieldTy->kind == K::Named && !fieldTy->declaration &&
        val->getType()->isPointerTy()) {
        if (llvm::Value* r = emitVtablelessBuiltinToString(val, fieldTy))
            return r;
    }
    // §8.5 (#1118 fallback): an unresolved generic-T operand whose substitution
    // is unavailable but whose runtime value is a heap object (class/interface
    // pointer) — dispatch through the dynamic ToString (override else type name),
    // the same path the interface branch uses, instead of mis-reading the object
    // as a bz_string.
    if (fieldTy && fieldTy->kind == K::Named && !fieldTy->declaration &&
        val->getType()->isPointerTy()) {
        if (llvm::Value* r = lowerObjectToString(val)) return r;
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
// fix #5943 (§27.7.1 "Equality of function values" / §8.1, v0.338). A function
// type is an `Object` (§8.1), so `Object`'s three members are callable on a
// function-typed receiver, and §27.7.1 gives `Equals` the function-value
// equality with a hash that agrees with it. No call arm knew the receiver
// kind, so the call did not lower (the #2434 / #3079 degrade). The receiver's
// object view (#5978) carries the shared function-value vtable, whose slots
// are exactly these three members, so the call is the ordinary `Object`
// dispatch over that view. Declines (null) for every other shape: a `?.`
// call, a different member or arity, a receiver that is not function-typed
// at this instantiation.
bool Emitter::isFnValObjectMemberCall(const ast::CallExpr& call) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target || !ma->typeArgs.empty()) return false;
    const bool isHash = ma->member == "GetHashCode" && call.args.empty();
    const bool isToStr = ma->member == "ToString" && call.args.empty();
    const bool isEq = ma->member == "Equals" && call.args.size() == 1 &&
                      call.args[0].value && call.args[0].modifier.empty();
    if (!isHash && !isToStr && !isEq) return false;
    const sema::TypeRef* recvTy =
        resolveTypeParamRef(ctx_.exprType(*ma->target));
    return recvTy && recvTy->kind == sema::TypeRef::Kind::Function &&
           !recvTy->nullable;
}

llvm::Value* Emitter::lowerFnValObjectMemberCall(const ast::CallExpr& call) {
    if (!isFnValObjectMemberCall(call)) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    const bool isHash = ma->member == "GetHashCode";
    const bool isToStr = ma->member == "ToString";
    // A receiver (or argument) that is ITSELF a forming expression —
    // `Util.Twice.GetHashCode()`, `f.Equals(lambda …)` — yields a fresh cell
    // nothing binds; these members keep no reference, so it is released
    // once the answer is computed. `lastStampedFnCell_` is the proof of
    // freshness: reset here, it can only name a cell stamped while lowering
    // this operand, and it equals the operand's value only when the operand
    // is that cell's own forming expression (a read of a local is a load).
    // A lambda LITERAL here is a temporary exactly as one in an argument
    // position is (#3778): the same flag moves its captured cells' release
    // to the captured local's own scope.
    const bool savedInArg = inArgumentLambda_;
    auto lowerOperand = [&](const ast::Expr& oe) {
        const bool lit = dynamic_cast<const ast::LambdaExpr*>(&oe) != nullptr;
        if (lit) inArgumentLambda_ = true;
        llvm::Value* ov = lowerExpr(oe);
        inArgumentLambda_ = savedInArg;
        // `collectCellFreeEligible` classed the literal non-escaping, so
        // `lowerLambda` queued its captured cells: arm their scope-end free.
        if (lit) consumeLastLambdaCapturedCells();
        return ov;
    };
    lastStampedFnCell_ = nullptr;
    llvm::Value* cell = lowerOperand(*ma->target);
    if (!cell || !cell->getType()->isPointerTy()) return nullptr;
    llvm::Value* freshRecv =
        (cell == static_cast<llvm::Value*>(lastStampedFnCell_)) ? cell
                                                                : nullptr;
    llvm::Value* recv = fnValToObjectView(cell);
    if (isHash || isToStr) {
        llvm::Value* r = isHash ? lowerObjectGetHashCode(recv)
                                : lowerObjectToString(recv);
        if (freshRecv) emitClosureRelease(freshRecv);
        return r;
    }
    const ast::Expr& argE = *call.args[0].value;
    lastStampedFnCell_ = nullptr;
    llvm::Value* other = lowerOperand(argE);
    if (!other) return nullptr;
    llvm::Value* freshArg =
        (other == static_cast<llvm::Value*>(lastStampedFnCell_)) ? other
                                                                 : nullptr;
    llvm::Value* r = nullptr;
    if (!other->getType()->isPointerTy()) {
        // A value-typed argument is no function value: §27.7.1's equality
        // holds between function values only, so the answer is `false` (the
        // argument was still evaluated above).
        r = builder_.getInt1(false);
    } else {
        const sema::TypeRef* argTy = resolveTypeParamRef(ctx_.exprType(argE));
        if (argTy && argTy->kind == sema::TypeRef::Kind::Function)
            other = fnValToObjectView(other);
        r = lowerObjectEquals(recv, other);
    }
    if (freshArg) emitClosureRelease(freshArg);
    if (freshRecv) emitClosureRelease(freshRecv);
    return r;
}

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
    //
    // fix #2846 / §28.9 (v0.240): RETAIN it. This function's contract — relied
    // on by every one of its callers — is that the value it hands back is a
    // fresh `+1` the consumer discharges once: `emitPrimitiveToString`'s own
    // declaration calls it "allocates a fresh heap string with rc := 1" and
    // routes `P::Object` straight here; `StringBuilder.Append(o: Object)` sets
    // `rendered = true` and TRANSFERS (no retain) precisely because of it; the
    // interpolation render path does the same. The other two arms honour that
    // (the vtable thunks mint, a literal is an immortal sentinel on which
    // retain/release are no-ops) — this arm did not, and returning the
    // receiver's own un-retained pointer to a consumer that releases it is a
    // heap-use-after-free, not a leak. MEASURED on b0fa41a: `var o: Object := s;
    // var r := $"[{o}]";` in a 20-iteration loop aborts with
    // "AddressSanitizer: heap-use-after-free … freed by bz_string_release" on
    // the SECOND iteration's read of `s`. Filed separately as the UAF it is; the
    // retain lives here because #2846's fix (recognizing `Object.ToString()` as
    // a +1 producer, LlvmBackend_Setup.cpp) is unsound without it — it would
    // turn that shape's balanced retain/release into a bare release.
    // `bz_string_retain` is null-safe and a no-op on the literal sentinel.
    builder_.SetInsertPoint(strBB);
    llvm::Value* strResult = recv;
    emitStringRetain(strResult);
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
    // #2066 / §4.6 / §28.9: track whether `acc` currently holds a fresh +1
    // producer (a previous emitStringConcat result) or the initial literal
    // (a sentinel — release is a no-op). Each emitStringConcat mints a fresh
    // rc=1 bz.string whose DATA is a byte-copy of its operands; the previous
    // `acc` is now only referenced by the caller (this accumulator slot) —
    // without a mid-chain release, every intermediate concat buffer leaks
    // (a multi-field record's ToString produces one intermediate per concat
    // step plus one per value-typed field's ToString). This mirrors BinaryExpr
    // `+`'s releaseConcatIntermediate (LlvmBackend_Exprs.cpp): release the
    // previous `acc` AFTER the concat has produced its +1 result so the
    // intermediate is fully consumed first — no UAF, no double-free. The
    // initial literal has sentinel rc, so its release is a no-op; subsequent
    // `acc` values are real +1 producers released exactly once here. The
    // FINAL `acc` is returned to the caller (the +1 it binds) and is NOT
    // released here.
    bool accIsPlusOne = false;   // `lowerStringLiteral` returns a sentinel.
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(rec, storage);
    unsigned idx = 0;       // struct element index
    bool first = true;
    using K = sema::TypeRef::Kind;
    using P = sema::PrimitiveType;
    for (const sema::Symbol* m : storage) {
        if (!first) {
            llvm::Value* prev = acc;
            acc = emitStringConcat(acc, lowerStringLiteral("\", \"", false));
            if (accIsPlusOne) emitStringRelease(prev);
            accIsPlusOne = true;
        }
        first = false;
        // `PropName: ` prefix — the member's declared name verbatim (§9.1:
        // "the compiler does not transform them").
        {
            llvm::Value* prev = acc;
            acc = emitStringConcat(acc, lowerStringLiteral(
                std::string("\"") + std::string(m->name) + ": \"", false));
            if (accIsPlusOne) emitStringRelease(prev);
            accIsPlusOne = true;
        }
        llvm::Value* fieldVal = builder_.CreateExtractValue(recVal, {idx});
        // issue #462 / §14.1: a generic record's `T`-typed field formats by its
        // CONCRETE type under the active subst (a string renders its value, a
        // record recurses) — not the type-erased shape fallback.
        const sema::TypeRef* fieldTy = recordMemberTypeResolved(m);
        llvm::Value* fieldStr = emitValueToString(fieldVal, fieldTy);
        {
            llvm::Value* prev = acc;
            acc = emitStringConcat(acc, fieldStr);
            if (accIsPlusOne) emitStringRelease(prev);
            accIsPlusOne = true;
        }
        // #2066 / §4.6 / §28.9: a value-typed field's ToString is a fresh +1
        // producer (int via bz_int_to_string, double via bz_double_to_string,
        // char via bz_char_to_string, decimal via bz_decimal_to_string, a
        // nested record's final concat, a boxed Object dispatch, a class
        // ToString override). Now that the concat above has consumed it
        // (made its own +1 byte-copy), release the field's +1 — mirroring
        // BinaryExpr `+`'s releaseConcatIntermediate. A literal-form result
        // (bool.ToString → "True"/"False" sentinel; class without ToString →
        // type-name sentinel) has sentinel rc, so its release is a no-op.
        // A STRING field's ToString is IDENTITY (§9.6 — `emitPrimitiveToString`
        // returns `val` itself): it is a BORROW of the record's own slot, not
        // a +1, and must NOT be released (the record still owns it; releasing
        // would double-free on the record's own disposal). The same applies
        // to a `string?` field whose non-null branch returns the borrow.
        bool fieldStrIsBorrow = fieldTy &&
            fieldTy->kind == K::Primitive &&
            fieldTy->primitive == P::String;
        if (!fieldStrIsBorrow) emitStringRelease(fieldStr);
        ++idx;
    }
    {
        llvm::Value* prev = acc;
        acc = emitStringConcat(acc, lowerStringLiteral("\")\"", false));
        if (accIsPlusOne) emitStringRelease(prev);
        // `acc` is the FINAL +1 result the caller binds — keep it.
    }
    return acc;
}

// §35.7 (#2479): build `"TypeName.CaseName(field1: value1, …)"` for the
// ACTIVE case of an auto-hashable choice, or `"TypeName.CaseName"`
// for a payload-free case — the exact format §35.7 specifies. Callers are
// responsible for confirming the choice is auto-hashable first
// (via `sema::isAutoEqualityBearingChoice`, mirroring how a caller must
// confirm value-likeness before `emitChoiceEquality`/`emitChoiceHashCode`);
// this function does not re-check it, matching those two siblings.
//
// REAL CONTROL FLOW (the emitChoiceEquality/emitChoiceHashCode shape),
// NOT a select chain: unlike those two, EVERY case here — payload-free
// included — needs its OWN distinct literal (`"TypeName.Red"` differs from
// `"TypeName.Blue"`), so no case can collapse into the switch's default arm
// the way a payload-free case does for equality/hash (which only need "is
// the tag equal" / "the tag alone"). Each case gets its own switch arm and
// basic block; only the active case's formatting code actually executes
// (a straight-line emit would run every case's field formatting
// unconditionally, reading the inactive cases' `undef` payload slots — the
// exact hazard emitChoiceEquality's own header comment documents). The
// switch's default (an out-of-range tag; defensive only, never reachable
// for a validly-constructed value) yields the bare type name.
llvm::Value* Emitter::emitChoiceToString(llvm::Value* val,
                                         const sema::TypeSymbol* choiceTy,
                                         const sema::TypeRef* instTy) {
    llvm::Value* tag = builder_.CreateExtractValue(val, {0u}, "chts.tag");
    auto* entryBB = builder_.GetInsertBlock();
    auto* endBB = llvm::BasicBlock::Create(llvm_, "chts.end", currentFn_);
    auto cases = choiceCasesOf(choiceTy);
    auto* sw = builder_.CreateSwitch(tag, endBB,
                                     static_cast<unsigned>(cases.size()));
    std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>> incoming;
    incoming.emplace_back(
        entryBB,
        lowerStringLiteral(std::string("\"") + std::string(choiceTy->name) +
                                "\"",
                            false));
    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        const sema::ChoiceCaseSymbol* cc = cases[ci];
        if (!cc) continue;
        auto* armBB = llvm::BasicBlock::Create(llvm_, "chts.arm", currentFn_);
        sw->addCase(builder_.getInt32(static_cast<int>(ci)), armBB);
        builder_.SetInsertPoint(armBB);
        std::string prefix = std::string("\"") + std::string(choiceTy->name) +
                             "." + std::string(cc->name);
        if (!cc->caseDecl || cc->caseDecl->payload.empty()) {
            // Payload-free case (§35.7): "TypeName.CaseName" — a plain
            // literal, no field formatting, so no control-flow hazard.
            llvm::Value* caseStr = lowerStringLiteral(prefix + "\"", false);
            auto* armEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            incoming.emplace_back(armEnd, caseStr);
            continue;
        }
        // Payload-bearing case: "TypeName.CaseName(field1: value1, …)" —
        // the same accumulator/release discipline as emitRecordToString
        // above (each intermediate emitStringConcat result is a fresh +1
        // that must be released once consumed by the next concat; a
        // string-typed field's ToString is a BORROW of the payload slot,
        // per §9.6, and must not be released).
        llvm::Value* acc = lowerStringLiteral(prefix + "(\"", false);
        bool accIsPlusOne = false;
        for (std::size_t pi = 0; pi < cc->caseDecl->payload.size(); ++pi) {
            const ast::Param& pf = cc->caseDecl->payload[pi];
            if (pi != 0) {
                llvm::Value* prev = acc;
                acc = emitStringConcat(acc,
                                       lowerStringLiteral("\", \"", false));
                if (accIsPlusOne) emitStringRelease(prev);
                accIsPlusOne = true;
            }
            {
                llvm::Value* prev = acc;
                acc = emitStringConcat(
                    acc, lowerStringLiteral(
                             std::string("\"") + std::string(pf.name) +
                                 ": \"",
                             false));
                if (accIsPlusOne) emitStringRelease(prev);
                accIsPlusOne = true;
            }
            llvm::Value* fieldVal = builder_.CreateExtractValue(
                val, {static_cast<unsigned>(1 + ci),
                      static_cast<unsigned>(pi)}, "chts.f");
            sema::TypeRef pTy;  // resolved payload type for emitValueToString
            if (pf.type) {
                // fix #4278: when `instTy` is provided, use substituted
                // payload type; otherwise fall back to resolveTypeParamRef.
                if (instTy && !instTy->genericArgs.empty()) {
                    const sema::TypeRef* rawTy = ctx_.resolvedType(*pf.type);
                    if (rawTy && rawTy->kind == sema::TypeRef::Kind::Named &&
                        !rawTy->declaration) {
                        std::string_view head = rawTy->headName();
                        for (std::size_t i = 0;
                             i < choiceTy->genericParams.size(); ++i) {
                            if (choiceTy->genericParams[i] &&
                                choiceTy->genericParams[i]->name == head &&
                                i < instTy->genericArgs.size()) {
                                pTy = *instTy->genericArgs[i];
                                break;
                            }
                        }
                    }
                    if (pTy.kind == sema::TypeRef::Kind::Error) {
                        if (rawTy) pTy = *rawTy;
                    }
                } else {
                    if (const sema::TypeRef* rt =
                            resolveTypeParamRef(ctx_.resolvedType(*pf.type)))
                        pTy = *rt;
                }
            }
            llvm::Value* fieldStr = emitValueToString(fieldVal, &pTy);
            {
                llvm::Value* prev = acc;
                acc = emitStringConcat(acc, fieldStr);
                if (accIsPlusOne) emitStringRelease(prev);
                accIsPlusOne = true;
            }
            bool fieldStrIsBorrow =
                pTy.kind == sema::TypeRef::Kind::Primitive &&
                pTy.primitive == sema::PrimitiveType::String;
            if (!fieldStrIsBorrow) emitStringRelease(fieldStr);
        }
        {
            llvm::Value* prev = acc;
            acc = emitStringConcat(acc, lowerStringLiteral("\")\"", false));
            if (accIsPlusOne) emitStringRelease(prev);
            // `acc` is this arm's final +1 result — keep it for the PHI.
        }
        auto* armEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        incoming.emplace_back(armEnd, acc);
    }
    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(ptrTy(),
                                   static_cast<unsigned>(incoming.size()),
                                   "chts");
    for (auto& [bb, v] : incoming) phi->addIncoming(v, bb);
    return phi;
}

// fix #2661 (spec §8.5, v0.252): the §8.5 POSITIONAL-RECORD default for the
// standard library's span records. §8.5 states the universality directly:
//
//   "The defaults above are generated for **every** type of the applicable
//    kind, wherever declared — user modules and the standard library alike. The
//    standard library's positional records (`Span<T>`, `ReadOnlySpan<T>`,
//    `ArraySpan<T>`, `ReadOnlyArraySpan<T>`, …) carry the positional-record
//    default like any other record"
//
// and stdlib §4.14 declares `public record Span<T>(Buffer: OwningBuffer<T>,
// Start: int, Length: int)` (§4.15: `ArraySpan<T>(Source: Array<T>, Start: int,
// Length: int)`), so the rendered shape is fully determined:
//
//   Span(Buffer: OwningBuffer, Start: 0, Length: 3)
//   ArraySpan(Source: Array, Start: 1, Length: 2)
//
// THE COMPONENT. §8.5: "Each component renders as its own `ToString` renders it
// (virtual dispatch — a class-typed component with no override renders its
// class default, the qualified name per the bullet above)". `OwningBuffer<T>` /
// `Array<T>` are reserved BUILT-IN classes with no user `TypeSymbol`, so their
// class default is the bare head today — the residual filed as #3119, the same
// one every other built-in-class rendering in this backend carries. It is
// emitted as a COMPILE-TIME LITERAL rather than by loading the component
// pointer, which is also what makes this rendering safe on a `default(Span<T>)`
// whose source pointer is null: nothing is dereferenced.
//
// §9.7 / §8.6 ARE UNAFFECTED, which is the property the #2311 report cared
// about: the receiver is read for two i32 fields and nothing else. No borrow is
// retained, consumed or extended, and the source pointer is never loaded — so
// the whole rendering is §8.5's "call-bounded borrow of the receiver and its
// components".
//
// The accumulator/release discipline is `emitRecordToString`'s verbatim (#2066
// / §4.6 / §28.9): each `emitStringConcat` mints a fresh rc=1 string and the
// previous accumulator is released once the concat has consumed it; the initial
// literal has the #422 sentinel rc so releasing it is a no-op; the FINAL
// accumulator is the +1 the caller binds.
llvm::Value* Emitter::emitSpanToString(llvm::Value* spanVal,
                                       std::string_view head) {
    if (!spanVal || !spanVal->getType()->isStructTy()) return nullptr;
    // stdlib §4.14 vs §4.15: the owning spans view an `OwningBuffer<T>` through
    // a component named `Buffer`; the borrowing spans view an `Array<T>`
    // through one named `Source`. Both lower to the same `bz.arrayspan`.
    const bool owning = (head == "Span" || head == "ReadOnlySpan");
    // ⚠ #3119 MARKER — these two literals ARE the component's §8.5 class
    // default, spelled out. `OwningBuffer<T>` / `Array<T>` are reserved
    // built-ins with no declaring module and no type-argument rendering, so
    // #3119's fix ("System.Collections.Generic.OwningBuffer<int>") has to
    // change THEM; nothing routes through a shared renderer that would pick the
    // change up for free. Grep `#3119` before assuming this file is done.
    const char* firstComponent =
        owning ? "Buffer: OwningBuffer"     // #3119: unqualified, no <T>
               : "Source: Array";           // #3119: unqualified, no <T>

    llvm::Value* startV = builder_.CreateExtractValue(spanVal, {1u}, "sp.ts.st");
    llvm::Value* lenV   = builder_.CreateExtractValue(spanVal, {2u}, "sp.ts.len");

    llvm::Value* acc = lowerStringLiteral(
        std::string("\"") + std::string(head) + "(" + firstComponent +
            ", Start: \"",
        false);
    bool accIsPlusOne = false;   // `lowerStringLiteral` returns a sentinel.
    auto appendStr = [&](llvm::Value* s, bool releaseS) {
        llvm::Value* prev = acc;
        acc = emitStringConcat(acc, s);
        if (accIsPlusOne) emitStringRelease(prev);
        accIsPlusOne = true;
        if (releaseS) emitStringRelease(s);
    };

    // `Start` and `Length` are `int` (stdlib §4.14/§4.15); each render is a
    // fresh +1 from `bz_int_to_string`, consumed by the concat and released.
    appendStr(emitPrimitiveToString(startV, sema::PrimitiveType::Int), true);
    appendStr(lowerStringLiteral("\", Length: \"", false), false);
    appendStr(emitPrimitiveToString(lenV, sema::PrimitiveType::Int), true);
    appendStr(lowerStringLiteral("\")\"", false), false);
    return acc;
}


// fix #2661 (spec §8.5, v0.252): the §8.5 AUTO-EQUALITY BODY-RECORD default for
// `Range<T>`. §8.5 names it explicitly, and names the bullet it takes:
//
//   "`Range<T>` (a compiler-intrinsic body record, stdlib §9.8) carries the
//    auto-equality body-record default of the bullet above, rendering its
//    auto-properties."
//
// — i.e. §8.5's "a representation of auto-properties, formatted as
// `"TypeName(Prop1: value1, Prop2: value2)"`", with the UNQUALIFIED head §8.5
// keeps for every record rendering. ⚠ NOT the positional-record bullet: the
// train's gate corrected an initial positional misclassification, and stdlib
// §9.8 declares `Range<T>` with a body (`public readonly property Start: T?;`
// …), not a positional parameter list.
//
// The auto-properties, in stdlib §9.8's declaration order:
//   Start: T?   End: T?   Step: long   IsEndInclusive: bool
//   IsOpenEnded: bool     IsEmpty: bool
//
// so `(1..<3).ToString()` renders
//   Range(Start: 1, End: 3, Step: 1, IsEndInclusive: False, IsOpenEnded: False,
//         IsEmpty: False)
//
// NULLABLE ENDPOINTS. `Start`/`End` are `T?` and §9.8 says "null if open-ended
// on the lower/upper side"; §8.5 defers to §18.5 for those — "a `null`-valued
// nullable component renders as the text `null`". The `bz.range` struct stores
// a 0 SENTINEL on an open side and records the openness in flags bits 1/2, so
// each endpoint is selected off its flag rather than rendered from the
// sentinel.
//
// THE DERIVED PROPERTIES ARE COMPUTED, NOT READ. `IsOpenEnded` and `IsEmpty`
// have no storage in `bz.range`; they are recomputed here from the same
// expressions the member-access lowering uses (LlvmBackend_Exprs.cpp's
// `isRangeType` block), so `r.IsEmpty` and the `IsEmpty:` component of
// `r.ToString()` cannot disagree — §16.3's emptiness rule including the
// v0.162/#1496 "an open-ended range is never empty" short-circuit and the
// step-direction test.
//
// ⚠ A RECORDED INTERPRETATION, not a settled reading (CLAUDE.md rule 4). §9.2
// says of the auto-`ToString` it defines: "equality and the auto-formatted
// representation are always over storage, never over derived values", and
// "Computed (expression-bodied) properties are not part of the equality
// comparison or the ToString output". Read strictly, `IsOpenEnded` and
// `IsEmpty` are derived and would be OMITTED — leaving a four-component
// rendering. All six are rendered here because §8.5's own sentence about this
// type is the more specific one and reads as a whole-surface instruction
// ("`Range<T>` … carries the auto-equality body-record default of the bullet
// above, rendering ITS AUTO-PROPERTIES"), and stdlib §9.8 declares all six
// identically as `public readonly property` with no body — nothing in the
// declaration marks a subset as computed, so "its auto-properties" has no
// smaller referent to mean. The tension is between §8.5's per-type sentence
// and §9.2's general one; it is filed as #3131 rather than resolved here, and
// if it is ruled the other way the fix is to drop the last two components and
// their test assertions.
//
// RESIDUAL. A BOXED range (`var o: Object := r; o.ToString()`) still renders
// nothing, because `Range<T>` has no box vtable at all — `boxVtableFor` has no
// arm for it, so the value is never boxed. That is a BOXING gap independent of
// this rendering (it predates it; there was nothing to render either way), and
// it is filed as #3127 with the shape a fix would take: an on-demand descriptor
// + vtable whose slot 3 forwards HERE, exactly as #2804 did for inline arrays
// and #2915 for the unit type.
llvm::Value* Emitter::emitRangeToString(llvm::Value* rangeVal,
                                        const sema::TypeRef* elemTy) {
    if (!rangeVal || !rangeVal->getType()->isStructTy()) return nullptr;
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    using P = sema::PrimitiveType;

    // fix #3242 (§8.5, v0.261): resolve the element TypeRef through the
    // active generic substitution. A Range arriving through unresolved
    // generic substitution (`Range<T>` in a generic body, e.g.
    // `Wrap<T>(r: Range<T>).ToString()`) carries `T` (a bare generic
    // parameter ref) as its element ref; the primitive-kind test below
    // answers false for a Named-without-declaration ref, falling to the
    // `P::Long` default and rendering the char endpoint as its code-point
    // ordinal (`Start: 97`). §8.5's component rule requires the endpoint
    // to render as its own ToString — a substituted `char` endpoint is a
    // `char`, not an `i64`. The contract of `emitRangeToString` is "render
    // this range value, knowing its concrete element type", so the
    // substitution lives HERE rather than at every caller (the §8.5
    // string-rendering arm just below, the `Range<T>.ToString()` member-
    // call arm in LlvmBackend_Exprs.cpp, and the box-vtable ToString thunk
    // in LlvmBackend_Setup.cpp all benefit) — matching #2503's
    // interpolation-side fix and every other codegen site that consults a
    // bare-T ref for rendering kind.
    const sema::TypeRef* resolvedElem = resolveTypeParamRef(elemTy);

    // §8.5's component rule applied to the `T?` endpoints (stdlib §9.8), whose
    // storage is a plain i64 in `bz.range` whatever T is. §9.8 restricts T to
    // the integer types and `char`, so exactly two of them need more than the
    // signed-64 rendering:
    //   * `char` — a Unicode scalar; `('a'..'z')` must render `Start: a`, not
    //     `Start: 97`. §16.3 makes `Range<char>` first-class ("Iteration over
    //     `Range<char>`"), so this is an ordinary instantiation, not an exotic
    //     one. `emitPrimitiveToString`'s #1282 `char` arm takes an i32 code
    //     point, so the i64 slot is truncated for it.
    //   * `ulong` — §10.3-style width: a value above `long.MaxValue` printed
    //     through `%lld` renders NEGATIVE. `%llu` is the same bits read right.
    // Every other admitted T (byte/sbyte/short/ushort/int/uint/long) renders
    // identically through the signed-64 path, so they share it rather than
    // growing a per-width arm that could drift.
    const P elemPrim =
        (resolvedElem && resolvedElem->kind == sema::TypeRef::Kind::Primitive)
            ? resolvedElem->primitive
            : P::Long;
    const P endpointPrim = (elemPrim == P::Char)  ? P::Char
                         : (elemPrim == P::ULong) ? P::ULong
                                                  : P::Long;

    llvm::Value* st   = builder_.CreateExtractValue(rangeVal, {0u}, "rts.st");
    llvm::Value* en   = builder_.CreateExtractValue(rangeVal, {1u}, "rts.en");
    llvm::Value* step = builder_.CreateExtractValue(rangeVal, {2u}, "rts.step");
    llvm::Value* flg  = builder_.CreateExtractValue(rangeVal, {3u}, "rts.flg");

    // flags bit 0 = IsEndInclusive; bit 1 = startOpen; bit 2 = endOpen.
    llvm::Value* incl = builder_.CreateTrunc(flg, i1(), "rts.incl");
    llvm::Value* startOpen = builder_.CreateICmpNE(
        builder_.CreateAnd(flg, llvm::ConstantInt::get(i8Ty, 2)),
        llvm::ConstantInt::get(i8Ty, 0), "rts.sopen");
    llvm::Value* endOpen = builder_.CreateICmpNE(
        builder_.CreateAnd(flg, llvm::ConstantInt::get(i8Ty, 4)),
        llvm::ConstantInt::get(i8Ty, 0), "rts.eopen");
    llvm::Value* isOpenEnded =
        builder_.CreateOr(startOpen, endOpen, "rts.oe");

    // §16.3's emptiness, mirroring the `IsEmpty` member-access lowering:
    // direction is set by the step's sign; an open-ended range is never empty.
    llvm::Value* stepNeg = builder_.CreateICmpSLT(
        step, llvm::ConstantInt::get(i64(), 0), "rts.stepNeg");
    llvm::Value* emptyAsc = builder_.CreateSelect(
        incl, builder_.CreateICmpSGT(st, en), builder_.CreateICmpSGE(st, en),
        "rts.ea");
    llvm::Value* emptyDesc = builder_.CreateSelect(
        incl, builder_.CreateICmpSLT(st, en), builder_.CreateICmpSLE(st, en),
        "rts.ed");
    llvm::Value* dirEmpty =
        builder_.CreateSelect(stepNeg, emptyDesc, emptyAsc, "rts.direm");
    llvm::Value* isEmpty = builder_.CreateSelect(
        isOpenEnded, builder_.getInt1(false), dirEmpty, "rts.empty");

    llvm::Value* acc = lowerStringLiteral("\"Range(Start: \"", false);
    bool accIsPlusOne = false;
    auto appendStr = [&](llvm::Value* s, bool releaseS) {
        llvm::Value* prev = acc;
        acc = emitStringConcat(acc, s);
        if (accIsPlusOne) emitStringRelease(prev);
        accIsPlusOne = true;
        if (releaseS) emitStringRelease(s);
    };
    // A `T?` endpoint: the text `null` when its openness flag is set (§18.5's
    // rule, which §8.5 defers to for a null component), else the integer.
    auto endpointStr = [&](llvm::Value* v, llvm::Value* open) -> llvm::Value* {
        auto* nullBB = llvm::BasicBlock::Create(llvm_, "rts.null", currentFn_);
        auto* valBB  = llvm::BasicBlock::Create(llvm_, "rts.val",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "rts.epend", currentFn_);
        builder_.CreateCondBr(open, nullBB, valBB);
        builder_.SetInsertPoint(nullBB);
        llvm::Value* nullStr = lowerStringLiteral("\"null\"", false);
        auto* nullEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(valBB);
        // The `char` renderer takes an i32 code point (#1282); the i64/unsigned
        // renderers take the slot as stored.
        llvm::Value* payload =
            (endpointPrim == P::Char) ? builder_.CreateTrunc(v, i32(), "rts.cp")
                                      : v;
        llvm::Value* valStr = emitPrimitiveToString(payload, endpointPrim);
        auto* valEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(ptrTy(), 2, "rts.ep");
        phi->addIncoming(nullStr, nullEnd);
        phi->addIncoming(valStr, valEnd);
        return phi;
    };

    // `emitStringRelease` is rc-aware and a NO-OP on the #422 sentinel, so the
    // endpoint PHI (a `bz_long_to_string` +1 on one arm, a literal sentinel on
    // the other) is discharged with one unconditional release — the same shape
    // #2880 records for `Console.WriteLine`'s nullable-value render.
    appendStr(endpointStr(st, startOpen), true);
    appendStr(lowerStringLiteral("\", End: \"", false), false);
    appendStr(endpointStr(en, endOpen), true);
    appendStr(lowerStringLiteral("\", Step: \"", false), false);
    appendStr(emitPrimitiveToString(step, P::Long), true);
    appendStr(lowerStringLiteral("\", IsEndInclusive: \"", false), false);
    appendStr(emitPrimitiveToString(incl, P::Bool), true);
    appendStr(lowerStringLiteral("\", IsOpenEnded: \"", false), false);
    appendStr(emitPrimitiveToString(isOpenEnded, P::Bool), true);
    appendStr(lowerStringLiteral("\", IsEmpty: \"", false), false);
    appendStr(emitPrimitiveToString(isEmpty, P::Bool), true);
    appendStr(lowerStringLiteral("\")\"", false), false);
    return acc;
}


// §8.3 / §9.1: hash a single field value of static type `fieldTy` → i32.
// Strings hash by content (consistent with content equality); nested records
// recurse; enums hash by their integer value; primitives by value.
llvm::Value* Emitter::emitValueHashCode(llvm::Value* val,
                                        const sema::TypeRef* fieldTy) {
    using K = sema::TypeRef::Kind;
    // Inline array (§8.3 / §9.2 v0.158): "Hash combining element hashes in slot
    // order." Fold each slot's hash (h = h*31 + elemHash), the same order and
    // combiner as the record/positional hash — consistent with the element-wise
    // equality of emitInlineArrayEquality so equal arrays hash identically.
    if (fieldTy && fieldTy->kind == K::InlineArray &&
        val->getType()->isArrayTy()) {
        auto* at = llvm::cast<llvm::ArrayType>(val->getType());
        llvm::Value* h = builder_.getInt32(17);   // non-zero seed
        for (unsigned i = 0; i < at->getNumElements(); ++i) {
            llvm::Value* ei = builder_.CreateExtractValue(val, {i});
            llvm::Value* eh = emitValueHashCode(ei, fieldTy->arrayElement);
            h = builder_.CreateAdd(
                builder_.CreateMul(h, builder_.getInt32(31)), eh);
        }
        return h;
    }
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
    //
    // fix #4281/#4282 (§8.3/§9.1, v0.298): this nullable-value dispatch MUST run
    // before the "Nested record" arm below — isNullableValueTy already includes
    // isNullableRecordTy (a `record?` field lowers to the SAME {i8 hasValue; T}
    // tagged struct as `int?`/an enum, LlvmBackend_impl.hpp's isNullableValueTy),
    // so a nullable record field is fully unwrapped and recursed with its inner
    // non-nullable type HERE, and the "Nested record" arm below only ever sees
    // an already-unwrapped, genuinely non-nullable record value. Previously this
    // block sat AFTER the "Nested record" arm (the only one of the three
    // nested-field recursion sites — emitValueEquality / emitValueHashCode /
    // emitValueToString — ordered that way; the other two already dispatch
    // nullable-ref/nullable-value before their own "Nested record" arm), so a
    // nullable record field's WRAPPER struct (`{i8 hasValue, Box$spec0}`) was
    // itself `isStructTy()` and satisfied the record arm's condition directly,
    // mis-treating the wrapper as the bare inner record: `recTy` became the
    // wrapper's struct type instead of the record's own, and the memoized
    // helper's body extracted the wrapper's `i8 hasValue` tag at field-index 0
    // believing it was the record's own first storage member.
    //   - issue 4282 (pre-existing, independent of issue 4251, zero generics
    //     involved — `record Mid(M: string); record SoloMid(BS: Mid?);`):
    //     `recordMemberTypeResolved` resolves `Mid.M` to the concrete `string`
    //     regardless of any substitution (Mid isn't generic), so the
    //     mis-extracted `i8` was routed into the string-content-hash arm and
    //     GEP'd as a `bz_string*` — IR-verifier crash, with zero generics
    //     involved, proving the defect is this ordering bug, not a generic-
    //     substitution gap.
    //   - issue 4281 (the issue-4251-era regression, `Box<T>` generic):
    //     pre-issue-4251, `RecordSubstGuard` was never installed on this path,
    //     so the mis-extracted tag's declared type (`Box`'s bare `T`) stayed
    //     unresolved and fell to the safe `isIntegerTy()` catch-all far below —
    //     wrong VALUE (a content-independent hash derived from the presence
    //     tag), but well-typed IR. Once issue 4251 started installing
    //     `RecordSubstGuard` at this same (mis-ordered) arm, `T` resolved to
    //     the concrete `string`, and the SAME mis-extracted `i8` was routed
    //     into the string-content-hash arm instead of the integer one —
    //     upgrading the latent wrong-value bug into the IR-verifier crash
    //     above. Moving this block ahead of the "Nested record" arm removes
    //     both: a nullable record field's wrapper is unwrapped by hasValue/
    //     nullablePrimValue right here, and the recursive call below reaches
    //     the "Nested record" arm with the record's own true struct value and
    //     its own true (non-nullable) declared type, matching
    //     emitValueEquality's already-correct order exactly.
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
    // Nested record (§9.1 transitivity): combine its own field hashes.
    //
    // fix #4234 (§9.1, v0.298): route through a per-INSTANTIATION MEMOIZED
    // helper (a real `call`) instead of re-inlining `emitRecordHashCode`'s
    // body here — the GetHashCode sibling of the identical ToString/Equals
    // fix (recordToStringHelperFn / recordEqualityHelperFn,
    // LlvmBackend_Setup.cpp; see recordToStringHelperFn's comment for why
    // the key/load type must be `val`'s OWN struct type, not one re-derived
    // from `fieldTy->declaration` alone): a shared nested record type
    // reached from `branch` sibling FIELD positions otherwise re-expands
    // its entire recursive hash combination at EVERY occurrence.
    //
    // fix #4281/#4282 (§8.3/§9.1, v0.298): `!fieldTy->nullable` is now
    // REDUNDANT with the nullable-value dispatch above (which already
    // intercepts and fully unwraps every nullable record field before this
    // point — a `record?` is always `isNullableValueTy`, see that arm's
    // comment) — but it is kept explicit here anyway, as a documented
    // invariant rather than one a reader must re-derive by tracing the whole
    // function's control flow: this arm must NEVER again see a nullable
    // record field's WRAPPER struct, because that is exactly the mis-
    // dispatch that produced issue 4281 (crash) and issue 4282 (silent wrong
    // hash) when this arm sat ahead of the nullable checks.
    if (fieldTy && fieldTy->kind == K::Named && fieldTy->declaration &&
        fieldTy->declaration->kind == ast::TypeDeclKind::Record &&
        !fieldTy->nullable &&
        val->getType()->isStructTy()) {
        // fix #4251 (§9.1/§14.1/§4.6, v0.298): install THIS field's own
        // generic substitution before the memoized helper's body is (first)
        // built — the GetHashCode sibling of the identical Equals gap (see
        // emitValueEquality's nested-record arm, LlvmBackend_Layout.cpp, for
        // the full mechanism). Pre-fix, `recordMemberTypeResolved` returned
        // Box's `V: T` field bare-unresolved under the empty ambient subst,
        // and NEITHER the string-hash arm (which requires `fieldTy->kind ==
        // Primitive`) nor any other typed arm matched a bare `Named`/no-decl
        // `T` — it fell all the way to this function's final catch-all
        // `return builder_.getInt32(0)`, a CONTENT-INDEPENDENT CONSTANT
        // (measured: `SoloStr(Box("hi")).GetHashCode() ==
        // SoloStr(Box("bye")).GetHashCode()`, both hashing to the same
        // value). `fieldTy` already carries the CONCRETE `genericArgs`
        // (`Box<string>`); a no-op for a non-generic nested record.
        RecordSubstGuard fieldSubst(this, fieldTy);
        auto* recTy = llvm::cast<llvm::StructType>(val->getType());
        llvm::Function* helper =
            recordHashCodeHelperFn(fieldTy->declaration, recTy);
        llvm::AllocaInst* spill = entryAlloca(recTy);
        builder_.CreateStore(val, spill);
        return builder_.CreateCall(helper->getFunctionType(), helper,
                                   {spill});
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
    // §9.2 v0.210 (#2083) / §35.7: a CHOICE-typed field hashes by active-case
    // tag + active-case payload fields (per emitChoiceHashCode). Dispatched
    // BEFORE the int/struct fall-through so a fresh choice value's payload
    // bits (which contain undef in inactive-case slots) don't dominate the
    // hash. Returns the case-switched fold described by §35.7: discriminant
    // mixed in first, then per-payload fields h = h*31 + fieldHash.
    if (fieldTy && fieldTy->kind == K::Named && fieldTy->declaration &&
        fieldTy->declaration->kind == ast::TypeDeclKind::Choice &&
        val->getType()->isStructTy()) {
        // fix #4278: pass fieldTy (the concrete instantiation) so that
        // emitChoiceHashCode can use substitutedChoicePayloadType to resolve
        // the generic payload type T -> concrete type (e.g. string).
        RecordSubstGuard fieldSubst(this, fieldTy);
        return emitChoiceHashCode(val, fieldTy->declaration, fieldTy);
    }
    // fix #4586 / §4.3.1 / §8.3 (v0.306): the hash half of the §8.3
    // `a.Equals(b) ⇒ a.GetHashCode() = b.GetHashCode()` invariant. §4.3.1
    // makes `=`/`Equals` reflexive for floats — "NaN = NaN is true", and
    // `-0.0 = +0.0` is true — so every NaN bit pattern (signaling or quiet,
    // any payload) MUST hash to the same value, and so must the two zeros,
    // or a `Set<double>`/`Dictionary<double, V>` lookup with a NaN/negative-
    // zero key that hashes to a different bucket than the one it was
    // inserted under silently misses even though `Equals` says it matches.
    // Canonicalize BEFORE taking the bits: a NaN operand (isNaN via `fcmp
    // uno x, x`, mirroring emitValueEquality's fix #1165 arm) becomes the
    // canonical quiet NaN; a zero operand (oeq against +0.0, true for both
    // signs) becomes +0.0. A canonicalized double still needs the same
    // low-32-bits-only fold as before it went through the raw bitcast/trunc
    // pair, so ordinary (non-NaN, non-zero) values hash exactly as they did.
    if (val->getType()->isDoubleTy()) {
        llvm::Value* isNan  = builder_.CreateFCmpUNO(val, val, "h.dnan");
        llvm::Value* isZero = builder_.CreateFCmpOEQ(
            val, llvm::ConstantFP::get(val->getType(), 0.0), "h.dzero");
        llvm::Value* norm = builder_.CreateSelect(
            isZero, llvm::ConstantFP::get(val->getType(), 0.0), val, "h.dnz");
        norm = builder_.CreateSelect(
            isNan, llvm::ConstantFP::getNaN(val->getType()), norm, "h.dnn");
        return builder_.CreateTrunc(
            builder_.CreateBitCast(norm, i64()), i32());
    }
    if (val->getType()->isFloatTy()) {
        llvm::Value* isNan  = builder_.CreateFCmpUNO(val, val, "h.fnan");
        llvm::Value* isZero = builder_.CreateFCmpOEQ(
            val, llvm::ConstantFP::get(val->getType(), 0.0), "h.fzero");
        llvm::Value* norm = builder_.CreateSelect(
            isZero, llvm::ConstantFP::get(val->getType(), 0.0), val, "h.fnz");
        norm = builder_.CreateSelect(
            isNan, llvm::ConstantFP::getNaN(val->getType()), norm, "h.fnn");
        return builder_.CreateBitCast(norm, i32());
    }
    // per §9.6 / stdlib §2.6 (#1464): a STRUCT-shaped value with no declared
    // record type — a value-builtin like `Guid` ({i64,i64}) whose declaration
    // is a builtin Class, not a Record, so the record branch above did not
    // catch it — folds its field hashes in slot order (h = h*31 + fieldHash),
    // the same combiner emitRecordHashCode uses. Without this a Guid key hashed
    // to the constant 0 (all keys collide in one bucket); folding the fields
    // keeps the hash consistent with the structural equality of the eq
    // trampoline so equal keys hash identically and distinct keys spread.
    if (auto* st = llvm::dyn_cast<llvm::StructType>(val->getType())) {
        llvm::Value* h = builder_.getInt32(17);   // non-zero seed
        for (unsigned i = 0; i < st->getNumElements(); ++i) {
            llvm::Value* fi = builder_.CreateExtractValue(val, {i});
            llvm::Value* fh = emitValueHashCode(fi, nullptr);
            h = builder_.CreateAdd(
                builder_.CreateMul(h, builder_.getInt32(31)), fh);
        }
        return h;
    }
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
    // §9.2 (v0.307) / fix #4643: skip equality-skip members in the hash the
    // same way emitStructEquality does — the invariant that equal records
    // hash equal must hold for content-only equality, so a skipped field is
    // not folded into the hash either. The physical extractvalue index
    // (`idx`) advances on every layout slot; only non-skipped members
    // contribute to the accumulator.
    std::vector<const sema::Symbol*> equalityStorage;
    collectRecordStorageForEquality(rec, equalityStorage);
    unsigned idx = 0;
    size_t eqIdx = 0;
    for (const sema::Symbol* m : storage) {
        llvm::Value* fieldVal = builder_.CreateExtractValue(recVal, {idx});
        if (!memberHasEqualitySkip(m)) {
            const sema::Symbol* eqM = (eqIdx < equalityStorage.size())
                                          ? equalityStorage[eqIdx++]
                                          : m;
            // issue #462 / §14.1: a generic record's `T`-typed field hashes by
            // its CONCRETE type under the active subst (a string hashes by
            // CONTENT, so the hash-equality invariant holds for content-equal
            // values) — not by pointer bits via the type-erased shape fallback.
            llvm::Value* fh = emitValueHashCode(
                fieldVal, recordMemberTypeResolved(eqM));
            llvm::Value* h = builder_.CreateLoad(i32(), hashSlot);
            h = builder_.CreateAdd(builder_.CreateMul(h, builder_.getInt32(31)),
                                   fh);
            builder_.CreateStore(h, hashSlot);
        }
        ++idx;
    }
    // §9.2: zero-storage record keeps the fixed seed constant.
    return builder_.CreateLoad(i32(), hashSlot);
}


}  // namespace bezel::codegen
