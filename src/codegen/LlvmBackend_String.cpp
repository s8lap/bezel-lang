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


std::vector<uint32_t> Emitter::processStringCodePoints(
        std::string_view text, bool verbatim) {
    size_t start = verbatim ? 2 : 1;
    if (text.size() < (verbatim ? 3u : 2u)) return {};
    text = text.substr(start, text.size() - start - 1);

    std::vector<uint32_t> cps;
    cps.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (verbatim) {
            if (text[i] == '"' && i+1 < text.size() && text[i+1] == '"') {
                cps.push_back('"'); ++i;
            } else {
                // Raw chars in verbatim — treat as Latin-1 code points.
                cps.push_back((unsigned char)text[i]);
            }
            continue;
        }
        if (text[i] != '\\') { cps.push_back((unsigned char)text[i]); continue; }
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
    auto* entryBB = builder_.GetInsertBlock();
    auto* lenBB = llvm::BasicBlock::Create(llvm_, "str.eq.len", currentFn_);
    auto* cmpBB = llvm::BasicBlock::Create(llvm_, "str.eq.cmp", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "str.eq.end", currentFn_);
    // Fast path: same pointer → equal.
    llvm::Value* samePtr = builder_.CreateICmpEQ(l, r, "str.sameptr");
    builder_.CreateCondBr(samePtr, endBB, lenBB);
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
    // Merge.
    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(i1(), 3, "str.eq");
    phi->addIncoming(builder_.getTrue(), entryBB);
    phi->addIncoming(builder_.getFalse(), lenEnd);
    phi->addIncoming(cmpEq, cmpEnd);
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
    char kind = std::toupper(spec[0]);
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
    if (kind == 'F' && isFloat) {
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "6") + "f";
    } else if (kind == 'E' && isFloat) {
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "6") + "e";
    } else if ((kind == 'G') && isFloat) {
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "6") + "g";
    } else if (kind == 'X' && isInt) {
        // §18.5 X[n]: zero-padded hex to n digits minimum.
        char xch = (spec[0] == 'x') ? 'x' : 'X';
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
    if (isFloat) {
        llvm::Value* dv = val->getType()->isFloatTy()
            ? builder_.CreateFPExt(val, llvm::Type::getDoubleTy(llvm_))
            : val;
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
                                   const ast::MemberAccessExpr& ma) {
    const auto& method = ma.member;
    if (!ma.target) return nullptr;
    llvm::Value* self = lowerExpr(*ma.target);
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
        auto* entBB  = builder_.GetInsertBlock();
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
            llvm::Value* sdata = loadStringData(s);
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

    // §9.6: StartsWith(prefix: string): bool
    // prefix.len <= self.len AND memcmp(self.data, prefix.data, prefix.len)==0
    if (method == "StartsWith") {
        llvm::Value* prefix = getStrArg(0);
        if (!prefix) return nullptr;
        llvm::Value* selfLen = loadStringLen(self);
        llvm::Value* prefLen = loadStringLen(prefix);
        // if prefLen > selfLen → false
        llvm::Value* tooLong = builder_.CreateICmpUGT(prefLen, selfLen, "sw.toolong");
        auto* cmpBB = llvm::BasicBlock::Create(llvm_, "sw.cmp", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "sw.end", currentFn_);
        auto* entryBB = builder_.GetInsertBlock();
        builder_.CreateCondBr(tooLong, endBB, cmpBB);
        builder_.SetInsertPoint(cmpBB);
        auto* memcmpFn = declareC("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* cmpRes = builder_.CreateCall(
            memcmpFn, {loadStringData(self), loadStringData(prefix), prefLen});
        llvm::Value* eqZ = builder_.CreateICmpEQ(cmpRes, llvm::ConstantInt::get(i32(),0));
        auto* cmpEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2, "sw.result");
        phi->addIncoming(builder_.getFalse(), entryBB);
        phi->addIncoming(eqZ, cmpEnd);
        return phi;
    }

    // §9.6: EndsWith(suffix: string): bool
    if (method == "EndsWith") {
        llvm::Value* suffix = getStrArg(0);
        if (!suffix) return nullptr;
        llvm::Value* selfLen = loadStringLen(self);
        llvm::Value* sufLen = loadStringLen(suffix);
        llvm::Value* tooLong = builder_.CreateICmpUGT(sufLen, selfLen, "ew.toolong");
        auto* cmpBB = llvm::BasicBlock::Create(llvm_, "ew.cmp", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "ew.end", currentFn_);
        auto* entryBB = builder_.GetInsertBlock();
        builder_.CreateCondBr(tooLong, endBB, cmpBB);
        builder_.SetInsertPoint(cmpBB);
        // offset = selfLen - sufLen; compare self.data+offset with suffix.data
        llvm::Value* offset = builder_.CreateSub(selfLen, sufLen, "ew.off");
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* selfOff = builder_.CreateGEP(i8Ty, loadStringData(self), offset);
        auto* memcmpFn = declareC("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* cmpRes = builder_.CreateCall(
            memcmpFn, {selfOff, loadStringData(suffix), sufLen});
        llvm::Value* eqZ = builder_.CreateICmpEQ(cmpRes, llvm::ConstantInt::get(i32(),0));
        auto* cmpEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2, "ew.result");
        phi->addIncoming(builder_.getFalse(), entryBB);
        phi->addIncoming(eqZ, cmpEnd);
        return phi;
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
                auto* entBB  = builder_.GetInsertBlock();
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
        llvm::Value* sub = getStrArg(0);
        if (!sub) return nullptr;
        // Declare memmem: void* memmem(void*, size_t, void*, size_t)
        auto* memmemFn = module_->getFunction("memmem");
        if (!memmemFn) {
            auto* ft = llvm::FunctionType::get(
                ptrTy(), {ptrTy(), i64(), ptrTy(), i64()}, false);
            memmemFn = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, "memmem", module_.get());
        }
        llvm::Value* found = builder_.CreateCall(memmemFn, {
            loadStringData(self), loadStringLen(self),
            loadStringData(sub),  loadStringLen(sub)});
        return builder_.CreateICmpNE(
            found, llvm::ConstantPointerNull::get(ptrTy()), "contains");
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
        llvm::Value* sub = getStrArg(0);
        if (!sub) return nullptr;
        auto* memmemFn = module_->getFunction("memmem");
        if (!memmemFn) {
            auto* ft = llvm::FunctionType::get(
                ptrTy(), {ptrTy(), i64(), ptrTy(), i64()}, false);
            memmemFn = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, "memmem", module_.get());
        }
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* found = builder_.CreateCall(memmemFn, {
            selfData, loadStringLen(self),
            loadStringData(sub), loadStringLen(sub)});
        llvm::Value* isNull = builder_.CreateICmpEQ(
            found, llvm::ConstantPointerNull::get(ptrTy()), "iof.null");
        // If found: index = (found ptr) - (selfData ptr), cast to i32.
        llvm::Value* diff = builder_.CreatePtrDiff(
            llvm::Type::getInt8Ty(llvm_), found, selfData, "iof.diff");
        llvm::Value* idx = builder_.CreateTrunc(diff, i32(), "iof.idx");
        return builder_.CreateSelect(isNull,
            llvm::ConstantInt::getSigned(i32(), -1), idx, "iof.result");
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
    // Implemented by scanning from the end using reverse memmem search loop.
    if (method == "LastIndexOf") {
        llvm::Value* sub = getStrArg(0);
        if (!sub) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* selfLen  = loadStringLen(self);
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* subLen   = loadStringLen(sub);
        llvm::Value* subData  = loadStringData(sub);
        auto* memmemFn = module_->getFunction("memmem");
        if (!memmemFn) {
            auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i64(), ptrTy(), i64()}, false);
            memmemFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "memmem", module_.get());
        }
        // Scan from offset 0..selfLen-subLen, track last match.
        auto* lastSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(-1), lastSlot);
        auto* posSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), posSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "liof.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "liof.body", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "liof.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* pos = builder_.CreateLoad(i64(), posSlot);
        llvm::Value* rem = builder_.CreateSub(selfLen, pos, "liof.rem");
        llvm::Value* hasSub = builder_.CreateICmpSGE(rem, subLen, "liof.hasub");
        builder_.CreateCondBr(hasSub, bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* posB = builder_.CreateLoad(i64(), posSlot);
        llvm::Value* remB = builder_.CreateSub(selfLen, posB, "liof.remb");
        llvm::Value* dataOff = builder_.CreateGEP(i8Ty, selfData, posB, "liof.off");
        llvm::Value* found = builder_.CreateCall(memmemFn, {dataOff, remB, subData, subLen}, "liof.found");
        auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* notFound = builder_.CreateICmpEQ(found, nullPtr);
        auto* updateBB = llvm::BasicBlock::Create(llvm_, "liof.upd", currentFn_);
        builder_.CreateCondBr(notFound, endBB, updateBB);
        builder_.SetInsertPoint(updateBB);
        llvm::Value* matchOff = builder_.CreatePtrDiff(i8Ty, found, selfData, "liof.moff");
        builder_.CreateStore(matchOff, lastSlot);
        // Advance past this match.
        llvm::Value* newPos = builder_.CreateAdd(matchOff, subLen, "liof.npos");
        builder_.CreateStore(newPos, posSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        llvm::Value* lastI64 = builder_.CreateLoad(i64(), lastSlot);
        return builder_.CreateTrunc(lastI64, i32(), "liof.result");
    }

    // §9.6: Substring(start) → new string from start to end.
    // §9.6: Substring(start, length) → new string of given length.
    if (method == "Substring") {
        llvm::Value* startV = getIntArg(0);
        if (!startV) return nullptr;
        llvm::Value* selfLen = loadStringLen(self);
        llvm::Value* start64 = builder_.CreateSExt(startV, i64(), "sub.start");
        llvm::Value* subLen = (call.args.size() >= 2 && call.args[1].value)
            ? builder_.CreateSExt(lowerExpr(*call.args[1].value), i64(), "sub.len")
            : builder_.CreateSub(selfLen, start64, "sub.len");
        // Clamp to avoid over-reading.
        llvm::Value* maxLen = builder_.CreateSub(selfLen, start64, "sub.maxlen");
        llvm::Value* tooLong = builder_.CreateICmpSGT(subLen, maxLen, "sub.toolong");
        subLen = builder_.CreateSelect(tooLong, maxLen, subLen, "sub.clamped");
        // Allocate new string data + object.
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* dataSize = builder_.CreateAdd(subLen, builder_.getInt64(1));
        llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSize}, "sub.data");
        llvm::Value* srcPtr = builder_.CreateGEP(
            i8Ty, loadStringData(self), start64, "sub.src");
        builder_.CreateCall(memcpyFn, {newData, srcPtr, subLen});
        llvm::Value* endPtr = builder_.CreateGEP(i8Ty, newData, subLen);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), endPtr);
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* newStr = builder_.CreateCall(
            mallocFn, {builder_.getInt64(strBytes)}, "sub.obj");
        builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
        builder_.CreateStore(subLen, builder_.CreateStructGEP(stringType(), newStr, 1));
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
            builder_.CreateStructGEP(stringType(), newStr, 2));
            initStringRc(newStr);  // #422: producer's reference, rc := 1
        return newStr;
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

    // §9.6: Replace(oldStr, newStr): string — replaces all occurrences of
    // `oldStr` with `newStr` in `self`. Uses memmem for finding occurrences;
    // builds the result via repeated concatenation in a loop.
    if (method == "Replace" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* oldStr = lowerExpr(*call.args[0].value);
        llvm::Value* newStr = lowerExpr(*call.args[1].value);
        if (!oldStr || !newStr) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        // Declare memmem.
        auto* memmemFn = module_->getFunction("memmem");
        if (!memmemFn) {
            auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i64(), ptrTy(), i64()}, false);
            memmemFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "memmem", module_.get());
        }
        // Walk the input, collecting prefix+new segments.
        // For simplicity emit a result built from:
        //   acc = "" ; pos = self.data ; end = self.data + self.len
        //   while (found = memmem(pos, end-pos, old.data, old.len)):
        //     acc = acc + str(pos..found) + newStr
        //     pos = found + old.len
        //   acc = acc + str(pos..end)
        // All in LLVM IR using a loop.
        llvm::Value* selfLen  = loadStringLen(self);
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* oldData  = loadStringData(oldStr);
        llvm::Value* oldLen   = loadStringLen(oldStr);
        llvm::Value* newData2 = loadStringData(newStr);
        llvm::Value* newLen   = loadStringLen(newStr);
        // Allocate result buffer: worst case self.len * (newLen / oldLen + 1)
        // For simplicity allocate 4x self.len + newLen as initial buffer.
        // Actually: build result as linked list of segments, then concat.
        // Simpler approach: use realloc-based appending.
        // Simplest for now: pre-allocate 4x self.len + 4*newLen bytes.
        llvm::Value* bound = builder_.CreateAdd(
            builder_.CreateMul(selfLen, builder_.getInt64(4)),
            builder_.CreateMul(newLen, builder_.getInt64(4)));
        bound = builder_.CreateAdd(bound, builder_.getInt64(1));
        llvm::Value* resBuf = builder_.CreateCall(mallocFn, {bound}, "repl.buf");
        auto* resLenSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), resLenSlot);
        auto* posSlot = entryAlloca(ptrTy());
        builder_.CreateStore(selfData, posSlot);
        llvm::Value* endPtr = builder_.CreateGEP(i8Ty, selfData, selfLen, "repl.end");
        // Loop.
        auto* condBB = llvm::BasicBlock::Create(llvm_, "repl.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "repl.body", currentFn_);
        auto* dumpBB = llvm::BasicBlock::Create(llvm_, "repl.dump", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "repl.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* pos    = builder_.CreateLoad(ptrTy(), posSlot);
        llvm::Value* remain = builder_.CreatePtrDiff(i8Ty, endPtr, pos, "repl.rem");
        llvm::Value* hasOld = builder_.CreateICmpSGT(oldLen, builder_.getInt64(0));
        llvm::Value* remGe  = builder_.CreateICmpSGE(remain, oldLen);
        llvm::Value* canSearch = builder_.CreateAnd(hasOld, remGe);
        builder_.CreateCondBr(canSearch, bodyBB, dumpBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* pos2    = builder_.CreateLoad(ptrTy(), posSlot);
        llvm::Value* remain2 = builder_.CreatePtrDiff(i8Ty, endPtr, pos2, "repl.rem2");
        llvm::Value* found   = builder_.CreateCall(memmemFn, {pos2, remain2, oldData, oldLen}, "repl.found");
        auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* notFound = builder_.CreateICmpEQ(found, nullPtr);
        auto* copyBB = llvm::BasicBlock::Create(llvm_, "repl.copy", currentFn_);
        builder_.CreateCondBr(notFound, dumpBB, copyBB);
        builder_.SetInsertPoint(copyBB);
        // Copy prefix: found - pos2 bytes.
        llvm::Value* prefLen = builder_.CreatePtrDiff(i8Ty, found, pos2, "repl.pref");
        llvm::Value* resLen  = builder_.CreateLoad(i64(), resLenSlot);
        llvm::Value* dst1    = builder_.CreateGEP(i8Ty, resBuf, resLen);
        builder_.CreateCall(memcpyFn, {dst1, pos2, prefLen});
        llvm::Value* resLen2 = builder_.CreateAdd(resLen, prefLen);
        // Copy newStr.
        llvm::Value* dst2    = builder_.CreateGEP(i8Ty, resBuf, resLen2);
        builder_.CreateCall(memcpyFn, {dst2, newData2, newLen});
        llvm::Value* resLen3 = builder_.CreateAdd(resLen2, newLen);
        builder_.CreateStore(resLen3, resLenSlot);
        // Advance pos past the old occurrence.
        llvm::Value* afterOld = builder_.CreateGEP(i8Ty, found, oldLen, "repl.after");
        builder_.CreateStore(afterOld, posSlot);
        builder_.CreateBr(condBB);
        // Dump remaining.
        builder_.SetInsertPoint(dumpBB);
        llvm::Value* pos3     = builder_.CreateLoad(ptrTy(), posSlot);
        llvm::Value* tailLen  = builder_.CreatePtrDiff(i8Ty, endPtr, pos3, "repl.tail");
        llvm::Value* resLen4  = builder_.CreateLoad(i64(), resLenSlot);
        llvm::Value* dst3     = builder_.CreateGEP(i8Ty, resBuf, resLen4);
        builder_.CreateCall(memcpyFn, {dst3, pos3, tailLen});
        llvm::Value* finalLen = builder_.CreateAdd(resLen4, tailLen);
        builder_.CreateStore(finalLen, resLenSlot);
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        // Null-terminate.
        llvm::Value* fLen     = builder_.CreateLoad(i64(), resLenSlot);
        llvm::Value* nulPtr2  = builder_.CreateGEP(i8Ty, resBuf, fLen);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr2);
        // Build result string object.
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* resStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "repl.obj");
        builder_.CreateStore(resBuf, builder_.CreateStructGEP(stringType(), resStr, 0));
        builder_.CreateStore(fLen, builder_.CreateStructGEP(stringType(), resStr, 1));
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
            builder_.CreateStructGEP(stringType(), resStr, 2));
            initStringRc(resStr);  // #422: producer's reference, rc := 1
        return resStr;
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

    // §9.6: Split — two of the three overloads are lowered here:
    //   Split(separator: char)   — split on a single code point
    //   Split(separator: string) — split on a substring delimiter
    // Both return owned Array<string>. Width=1 (ASCII/UTF-8 byte mode), matching
    // bz_string_split/ToString. The Split(separators: Array<char>) overload is
    // NOT yet lowered: it depends on Array<char>'s element storage being a
    // contiguous i32 buffer, which interacts with pre-existing Array<char>
    // read-path issues — deferred rather than risk a miscompile.
    if (method == "Split" && call.args.size() == 1 && call.args[0].value) {
        auto* argTy = ctx_.exprType(*call.args[0].value);
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* selfLen  = loadStringLen(self);

        // Split(separator: char): one separator code point. Reuse the any-char
        // splitter with a single-element separator buffer on the stack.
        if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
            argTy->primitive == sema::PrimitiveType::Char) {
            llvm::Value* sepCh = lowerExpr(*call.args[0].value);
            if (!sepCh) return nullptr;
            if (!sepCh->getType()->isIntegerTy(32))
                sepCh = builder_.CreateSExt(sepCh, i32());
            llvm::AllocaInst* sepSlot = entryAlloca(i32());
            builder_.CreateStore(sepCh, sepSlot);
            auto* fn = declareC("bz_string_split_anychar", ptrTy(),
                                {ptrTy(), i64(), ptrTy(), i64()});
            return builder_.CreateCall(
                fn, {selfData, selfLen, sepSlot, builder_.getInt64(1)},
                "split.char.result");
        }

        // Split(separator: string): substring delimiter. Guard the receiver
        // type: only lower a genuine string argument here. A non-string, non-
        // char argument (e.g. an Array<char> for the not-yet-supported set
        // overload) must NOT reach loadStringData, which would mis-GEP a non-
        // string value — fall through to nullptr (normal unsupported path).
        if (!argTy || argTy->kind != sema::TypeRef::Kind::Primitive ||
            argTy->primitive != sema::PrimitiveType::String)
            return nullptr;
        llvm::Value* delim = lowerExpr(*call.args[0].value);
        if (!delim) return nullptr;
        // Runtime signature (bezel_runtime.h): bz_string_split(s, s_len, delim,
        // delim_len) — 4 params {ptr,i64,ptr,i64}. The previous declaration had a
        // spurious leading ptr (5 params) while the call passes 4, producing
        // malformed IR (arg-count mismatch) and an ABI-mismatched call.
        auto* fn = declareC("bz_string_split", ptrTy(),
                            {ptrTy(), i64(), ptrTy(), i64()});
        llvm::Value* delimData = loadStringData(delim);
        llvm::Value* delimLen  = loadStringLen(delim);
        return builder_.CreateCall(fn, {selfData, selfLen, delimData, delimLen},
                                   "split.result");
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
        acc = emitStringConcat(acc,
                               emitValueToString(fieldVal, recordMemberType(m)));
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
        llvm::Value* fh = emitValueHashCode(fieldVal, recordMemberType(m));
        llvm::Value* h = builder_.CreateLoad(i32(), hashSlot);
        h = builder_.CreateAdd(builder_.CreateMul(h, builder_.getInt32(31)), fh);
        builder_.CreateStore(h, hashSlot);
        ++idx;
    }
    // §9.2: zero-storage record keeps the fixed seed constant.
    return builder_.CreateLoad(i32(), hashSlot);
}


}  // namespace bezel::codegen
