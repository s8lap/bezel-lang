#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

llvm::Value* Emitter::lowerExpr(const ast::Expr& e) {
    diverged_ = false;  // CG40: cleared per expression; a `throw` sets it
    // CG30: an enum constant (`Color.Green`) is its integer value.
    if (llvm::Value* ev = enumConstant(e)) return ev;
    if (llvm::Value* cv = constMemberValue(e)) return cv;
    if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
        // CG25: the literal's width comes from its resolved type
        // (e.g. an `L`-suffixed literal is i64).
        llvm::Type* ty = i32();
        if (auto* rt = ctx_.exprType(e))
            if (rt->kind == sema::TypeRef::Kind::Primitive)
                ty = lowerPrimitive(rt->primitive);
        return llvm::ConstantInt::get(
            ty, static_cast<std::uint64_t>(parseIntLiteral(lit->text)),
            /*isSigned=*/true);
    }
    if (auto* boolLit = dynamic_cast<const ast::BoolLiteralExpr*>(&e)) {
        return llvm::ConstantInt::get(i1(), boolLit->value ? 1 : 0);
    }
    // CG32: a char literal is its (i32) code point.
    if (auto* ch = dynamic_cast<const ast::CharLiteralExpr*>(&e)) {
        return llvm::ConstantInt::get(
            i32(), static_cast<std::uint64_t>(parseCharLiteral(ch->text)),
            /*isSigned=*/false);
    }
    // CG24: float/double literal — value parsed from the lexeme, type
    // (f32/f64) from the expr's resolved type (default double).
    if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
        llvm::Type* ty = llvm::Type::getDoubleTy(llvm_);
        if (auto* rt = ctx_.exprType(e))
            if (rt->kind == sema::TypeRef::Kind::Primitive)
                ty = lowerPrimitive(rt->primitive);
        std::string digits;
        for (char c : fl->text) {
            if (c == '_' || c == 'f' || c == 'F' || c == 'd' || c == 'D')
                continue;
            digits.push_back(c);
        }
        double v = digits.empty() ? 0.0 : std::strtod(digits.c_str(),
                                                      nullptr);
        return llvm::ConstantFP::get(ty, v);
    }
    // CG27a (§4.6): string and verbatim-string literals → global bz_string.
    if (auto* sl = dynamic_cast<const ast::StringLiteralExpr*>(&e))
        return lowerStringLiteral(sl->text, /*verbatim=*/false);
    if (auto* vs = dynamic_cast<const ast::VerbatimStringExpr*>(&e))
        return lowerStringLiteral(vs->text, /*verbatim=*/true);
    // CG27d (§18.5): interpolated string — concatenate all parts.
    // Literal parts are materialized as string globals. Expression parts
    // call emitPrimitiveToString (for primitives) or string identity (for
    // string-typed exprs); other types call their ToString() intrinsic.
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        // Start with the empty string and concat each part.
        llvm::Value* acc = lowerStringLiteral("\"\"", false);  // ""
        // FINDING [2] / §4.6 / §28.9: each step `acc := emitStringConcat(acc,pv)`
        // mallocs a fresh +1 result and the PREVIOUS accumulator becomes an
        // anonymous, single-owner intermediate with no scope-end release armed
        // for it (the named-local release path only covers LocalDecl nodes), so
        // it would leak — unboundedly in a loop. appendPart releases the prior
        // accumulator AFTER the new one is produced (it has been consumed by the
        // concat — no UAF, no double-free). The very first accumulator is the
        // `""` literal (sentinel rc → release is a no-op). The FINAL accumulator
        // is returned and is never released here (it flows to the target and
        // follows normal placement/escape rules). The part value `pv` is NOT
        // released here: it may be a borrow (a string-typed part is a shared
        // reference to a named local/field per §28.9).
        auto appendPart = [&](llvm::Value* pv) {
            llvm::Value* prev = acc;
            acc = emitStringConcat(acc, pv);
            emitStringRelease(prev);
        };
        for (const auto& part : is->parts) {
            llvm::Value* pv = nullptr;
            if (!part.literal.empty()) {
                // Literal part: process escape sequences (e.g. \n → newline,
                // \t → tab) that were stored as raw chars by splitInterpolation.
                // Wrap in quotes so processStringText's strip-quotes logic works.
                std::string content = processStringText(
                    "\"" + part.literal + "\"", false);
                llvm::Constant* dataArr = llvm::ConstantDataArray::getString(
                    llvm_, content, /*AddNull=*/true);
                unsigned id = stringLiteralIdx_++;
                auto* dataGV = new llvm::GlobalVariable(
                    *module_, dataArr->getType(), true,
                    llvm::GlobalValue::PrivateLinkage, dataArr,
                    ".bz.istr.data." + std::to_string(id));
                dataGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                dataGV->setAlignment(llvm::Align(1));
                llvm::Constant* lenC = llvm::ConstantInt::get(
                    i64(), content.size());
                // The literal part is built from a byte string → width 1.
                // #422: constant global literal — SENTINEL rc (never freed). The
                // struct now has 4 fields {data, len, width, rc}; supply all.
                llvm::Constant* widC = llvm::ConstantInt::get(
                    llvm::Type::getInt8Ty(llvm_), 1);
                llvm::Constant* strVal = llvm::ConstantStruct::get(
                    stringType(), {dataGV, lenC, widC, stringRcSentinel()});
                auto* strGV = new llvm::GlobalVariable(
                    *module_, stringType(), true,
                    llvm::GlobalValue::PrivateLinkage, strVal,
                    ".bz.istr." + std::to_string(id));
                pv = strGV;
            } else if (part.expr) {
                // Expression part: lower the expression, then convert to
                // string. If a format spec is present (§18.5), apply it.
                llvm::Value* ev = lowerExpr(*part.expr);
                if (!ev) continue;
                // §18.5: format spec → translate to snprintf format.
                if (!part.formatSpec.empty()) {
                    pv = emitWithFormatSpec(ev, part.formatSpec,
                                            ctx_.exprType(*part.expr));
                    if (pv) {
                        appendPart(pv);
                        continue;
                    }
                }
                auto* rt = ctx_.exprType(*part.expr);
                // CG-nullable: for nullable value types (primitives, and a
                // nullable enum — issue #364), extract the value. If null,
                // format as "null"; otherwise format the inner value (an enum's
                // live value is its type name per §8.5, via emitValueToString
                // with the non-nullable inner type).
                if (rt && isNullableValueTy(rt) && ev->getType()->isStructTy()) {
                    llvm::Value* hasVal = nullablePrimHasValue(ev);
                    llvm::Value* primVal = nullablePrimValue(ev);
                    auto* valBB = llvm::BasicBlock::Create(llvm_, "np.val", currentFn_);
                    auto* nullBB = llvm::BasicBlock::Create(llvm_, "np.null", currentFn_);
                    auto* endBB = llvm::BasicBlock::Create(llvm_, "np.end", currentFn_);
                    builder_.CreateCondBr(hasVal, valBB, nullBB);
                    builder_.SetInsertPoint(valBB);
                    sema::TypeRef innerTy = *rt;
                    innerTy.nullable = false;
                    llvm::Value* vStr = emitValueToString(primVal, &innerTy);
                    auto* valEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(nullBB);
                    llvm::Value* nStr = lowerStringLiteral("\"null\"", false);
                    auto* nullEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(endBB);
                    auto* phi = builder_.CreatePHI(ptrTy(), 2);
                    phi->addIncoming(vStr, valEnd);
                    phi->addIncoming(nStr, nullEnd);
                    pv = phi;
                } else if (rt && rt->kind == sema::TypeRef::Kind::Primitive) {
                    pv = emitPrimitiveToString(ev, rt->primitive);
                } else if (rt && rt->kind == sema::TypeRef::Kind::Named &&
                           rt->declaration &&
                           rt->declaration->kind == ast::TypeDeclKind::Enum) {
                    // §8.5: an enum interpolates as the type's qualified name
                    // (the default ToString), not its underlying integer.
                    pv = lowerStringLiteral(
                        std::string("\"") +
                        std::string(rt->declaration->name) + "\"", false);
                } else if (!rt || rt->kind != sema::TypeRef::Kind::Primitive) {
                    // Type not set or not primitive — infer from LLVM type.
                    using P = sema::PrimitiveType;
                    auto* llvmTy = ev->getType();
                    if (llvmTy->isIntegerTy(1))
                        pv = emitPrimitiveToString(ev, P::Bool);
                    else if (llvmTy->isIntegerTy(8) || llvmTy->isIntegerTy(16) ||
                             llvmTy->isIntegerTy(32))
                        pv = emitPrimitiveToString(ev, P::Int);
                    else if (llvmTy->isIntegerTy(64))
                        pv = emitPrimitiveToString(ev, P::Long);
                    else if (llvmTy->isDoubleTy())
                        pv = emitPrimitiveToString(ev, P::Double);
                    else if (llvmTy->isFloatTy())
                        pv = emitPrimitiveToString(ev, P::Float);
                    else if (llvmTy->isPointerTy()) {
                        // Could be string (use as-is) or class (call ToString).
                        if (rt && rt->kind == sema::TypeRef::Kind::Named &&
                            rt->declaration) {
                            // Class/interface: call ToString() if available.
                            auto* ts = rt->declaration;
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
                                std::vector<llvm::Value*> args{ev};
                                pv = emitMethodCall(toStr, args, false);
                            } else {
                                pv = ev;  // string — use as-is
                            }
                        } else {
                            pv = ev;  // string or unknown ptr — use as-is
                        }
                    }
                    // For struct (record value): spill to alloca, call ToString.
                    if (!pv && ev->getType()->isStructTy() && rt &&
                        rt->kind == sema::TypeRef::Kind::Named &&
                        rt->declaration) {
                        auto* ts = rt->declaration;
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
                        // §8.5 / §9.2: a positional / combined record, or an
                        // eligible body record, gets the synthesized
                        // property-listing ToString; an ineligible body record
                        // gets the type-name default.
                        bool positional = false;
                        if (auto* td =
                                dynamic_cast<const ast::TypeDecl*>(ts->decl))
                            positional = td->hasPositionalFields;
                        bool autoBearing =
                            positional ||
                            sema::isAutoEqualityBearingBodyRecord(ts);
                        if (toStr) {
                            // Spill the record value to an alloca so we have a ptr.
                            llvm::AllocaInst* spill = entryAlloca(ev->getType());
                            builder_.CreateStore(ev, spill);
                            std::vector<llvm::Value*> args{spill};
                            pv = emitMethodCall(toStr, args, false);
                        } else if (ts->kind == ast::TypeDeclKind::Record &&
                                   autoBearing) {
                            // §8.5 / §9.1 / §9.2: a record with no user ToString
                            // gets the synthesized `TypeName(Prop: value, ...)`
                            // form over its storage members.
                            pv = emitRecordToString(ev, ts);
                        } else {
                            // No ToString — use type name (§8.5).
                            pv = lowerStringLiteral(
                                std::string("\"") + std::string(ts->name) + "\"", false);
                        }
                    }
                }
            }
            if (pv) appendPart(pv);
        }
        return acc;
    }
    // CG23: the `null` literal. For nullable value types (int?, …, and a
    // nullable enum — issue #364) produce the tagged-struct null; for reference
    // types produce nullptr.
    if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
        if (auto* rt = ctx_.exprType(e))
            if (isNullableValueTy(rt))
                return nullableValueNull(rt);
        return llvm::ConstantPointerNull::get(ptrTy());
    }
    // CG37: `default(T)` is the zero value of T (0 / false / 0.0 / null).
    if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
        return llvm::Constant::getNullValue(lowerType(de->type.get()));
    }
    // CG23: postfix `!` (null-forgiving) — raise NullReferenceException
    // if the operand is null, otherwise yield it (§17.12).
    if (auto* bang = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        llvm::Value* v = bang->operand ? lowerExpr(*bang->operand)
                                       : nullptr;
        if (!v) return nullptr;
        // CG-nullable: nullable value type (int?, bool?, …, and a nullable
        // enum — issue #364) — check hasValue field and extract the value.
        if (bang->operand) {
            if (auto* rt = ctx_.exprType(*bang->operand)) {
                if (isNullableValueTy(rt)) {
                    llvm::Value* hasVal = nullablePrimHasValue(v);
                    emitGuardThrow(builder_.CreateNot(hasVal),
                                   "NullReferenceException");
                    return nullablePrimValue(v);
                }
            }
        }
        if (v->getType()->isPointerTy()) {
            emitGuardThrow(builder_.CreateICmpEQ(
                               v, llvm::ConstantPointerNull::get(ptrTy())),
                           "NullReferenceException");
        }
        return v;
    }
    // CG40: `throw` in expression position (§31.2). Evaluate the
    // exception (if any), store + unwind exactly like the statement
    // form, then mark the expression as diverged: the current block is
    // now terminated, so phi-building consumers exclude this branch.
    // A poison placeholder is returned so non-null checks pass; it is
    // never consumed (the block does not reach any join).
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        llvm::Value* exc = th->operand ? lowerExpr(*th->operand) : nullptr;
        // #397: pass the AST so emitThrowValue can detect a direct re-throw
        // `throw ex` in expression position (the one case that must not free
        // the caught exception — see emitThrowValue for the full rationale).
        emitThrowValue(exc, th->operand.get());
        diverged_ = true;
        return llvm::PoisonValue::get(ptrTy());
    }
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        return lowerBinary(*bin);
    }
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        return lowerUnary(*un);
    }
    if (auto* cond = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        return lowerConditional(*cond);
    }
    if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
        // CG42: `x?.M(args)` — a call whose callee is a null-conditional
        // member access skips the call when the receiver is null.
        if (call->callee) {
            if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(
                    call->callee.get()))
                return lowerNullCondCall(*call, *nm);
        }
        // CG27c/27f: intrinsic methods on string/primitive receivers.
        if (call->callee) {
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call->callee.get())) {
                if (ma->target) {
                    // Pre-warm lambda param type hint for the target identifier.
                    if (!ctx_.exprType(*ma->target)) {
                        if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                                ma->target.get())) {
                            sema::Symbol* sym = ctx_.exprSymbol(*tid);
                            auto hit = lambdaParamTypeHint_.find(sym);
                            if (hit != lambdaParamTypeHint_.end())
                                ctx_.setExprType(ma->target.get(), hit->second);
                        }
                    }
                    auto* tt = ctx_.exprType(*ma->target);
                    if (tt && tt->kind == sema::TypeRef::Kind::Primitive) {
                        if (tt->primitive == sema::PrimitiveType::String) {
                            // CG27f: stdlib §9.6 string methods.
                            if (auto* v = tryLowerStringMethod(*call, *ma))
                                return v;
                        }
                        // CG27c: ToString() on any primitive (no arg). §8.5/§8.6
                        // (#417): an OBJECT-typed receiver is NOT a value
                        // primitive — `o.ToString()` dispatches through the
                        // uniform vtable ToString slot (a boxed value / class) or
                        // returns a contained string. emitPrimitiveToString on an
                        // Object pointer would snprintf the pointer as "%d".
                        if (call->args.empty() && ma->member == "ToString" &&
                            tt->primitive != sema::PrimitiveType::Object) {
                            llvm::Value* recv = lowerExpr(*ma->target);
                            if (recv)
                                return emitPrimitiveToString(recv, tt->primitive);
                        }
                        if (call->args.empty() && ma->member == "ToString" &&
                            tt->primitive == sema::PrimitiveType::Object) {
                            llvm::Value* recv = lowerExpr(*ma->target);
                            if (llvm::Value* r = lowerObjectToString(recv))
                                return r;
                        }
                        // §6.3: FP predicates as instance methods (x.IsNaN(), x.IsFinite()).
                        if (call->args.empty()) {
                            using P = sema::PrimitiveType;
                            bool isFP = tt->primitive == P::Double || tt->primitive == P::Float;
                            if (ma->member == "IsNaN" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                return builder_.CreateFCmpUNO(recv, recv, "isnan");
                            }
                            if (ma->member == "IsInfinity" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                                if (!recv->getType()->isDoubleTy())
                                    recv = builder_.CreateFPExt(recv, dbl);
                                return builder_.CreateOr(
                                    builder_.CreateFCmpOEQ(recv, llvm::ConstantFP::getInfinity(dbl, false)),
                                    builder_.CreateFCmpOEQ(recv, llvm::ConstantFP::getInfinity(dbl, true)));
                            }
                            if (ma->member == "IsFinite" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                // IsFinite = !IsNaN && !IsInfinity
                                llvm::Value* isNaN = builder_.CreateFCmpUNO(recv, recv);
                                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                                if (!recv->getType()->isDoubleTy())
                                    recv = builder_.CreateFPExt(recv, dbl);
                                llvm::Value* isInf = builder_.CreateOr(
                                    builder_.CreateFCmpOEQ(recv, llvm::ConstantFP::getInfinity(dbl, false)),
                                    builder_.CreateFCmpOEQ(recv, llvm::ConstantFP::getInfinity(dbl, true)));
                                return builder_.CreateNot(builder_.CreateOr(isNaN, isInf), "isfinite");
                            }
                            if (ma->member == "Abs") {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                if (recv->getType()->isFloatingPointTy()) {
                                    auto* fn = llvm::Intrinsic::getDeclaration(
                                        module_.get(), llvm::Intrinsic::fabs, {recv->getType()});
                                    return builder_.CreateCall(fn, {recv}, "fabs");
                                } else {
                                    llvm::Value* isNeg = builder_.CreateICmpSLT(
                                        recv, llvm::ConstantInt::get(recv->getType(), 0));
                                    return builder_.CreateSelect(isNeg, builder_.CreateNeg(recv), recv, "abs");
                                }
                            }
                        }
                        // §9.x: ToString(format) — formatted numeric string.
                        if (call->args.size() == 1 && call->args[0].value &&
                            ma->member == "ToString") {
                            llvm::Value* recv = lowerExpr(*ma->target);
                            if (!recv) return nullptr;
                            // Try to extract the format specifier from a literal arg.
                            std::string bezelFmt;
                            if (auto* sl = dynamic_cast<const ast::StringLiteralExpr*>(
                                    call->args[0].value.get())) {
                                bezelFmt = processStringText(sl->text, false);
                            }
                            if (!bezelFmt.empty()) {
                                // Translate Bezel format spec to printf format.
                                // "Fn" / "fn" → "%.Nf" (fixed decimal)
                                // "En" / "en" → "%.Ne" (scientific)
                                // "Gn" / "gn" → "%.Ng" (general)
                                // "X" / "x" → "%X" / "%x" (hex integer)
                                // "D" / "d" → "%d" (decimal integer)
                                std::string printfFmt;
                                char spec = std::toupper(bezelFmt[0]);
                                std::string digits = bezelFmt.size() > 1 ? bezelFmt.substr(1) : "";
                                using P = sema::PrimitiveType;
                                bool isFP = tt->primitive == P::Double || tt->primitive == P::Float;
                                if ((spec == 'F' || spec == 'G' || spec == 'E') && isFP) {
                                    char pfspec = spec == 'F' ? 'f' : (spec == 'E' ? 'e' : 'g');
                                    printfFmt = "%." + (digits.empty() ? "2" : digits) + pfspec;
                                    // Widen float to double for snprintf.
                                    if (tt->primitive == P::Float)
                                        recv = builder_.CreateFPExt(recv, llvm::Type::getDoubleTy(llvm_));
                                } else if (spec == 'X' || spec == 'D') {
                                    // §18.5: D/X with digits = min width, zero-padded.
                                    // D5 → %05d, X4 → %04X.
                                    char pfspec2 = spec == 'X' ? 'X' : 'd';
                                    if (!digits.empty())
                                        printfFmt = std::string("%0") + digits + pfspec2;
                                    else
                                        printfFmt = spec == 'X' ? "%X" : "%d";
                                    if (!recv->getType()->isIntegerTy(32))
                                        recv = builder_.CreateFPToSI(recv, i32());
                                } else {
                                    return emitPrimitiveToString(recv, tt->primitive);
                                }
                                // Emit snprintf with the translated format.
                                llvm::Constant* fmtData = llvm::ConstantDataArray::getString(
                                    llvm_, printfFmt, true);
                                unsigned fid = stringLiteralIdx_++;
                                auto* fmtGV = new llvm::GlobalVariable(*module_, fmtData->getType(), true,
                                    llvm::GlobalValue::PrivateLinkage, fmtData, ".bz.ts.fmt." + std::to_string(fid));
                                fmtGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                                auto* snpFn = module_->getFunction("snprintf");
                                if (!snpFn) {
                                    auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), i64(), ptrTy()}, true);
                                    snpFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "snprintf", module_.get());
                                }
                                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                                llvm::Value* buf = entryAlloca(llvm::ArrayType::get(i8Ty, 64));
                                llvm::Value* bufPtr = builder_.CreateGEP(llvm::ArrayType::get(i8Ty, 64), buf, {builder_.getInt64(0), builder_.getInt64(0)});
                                llvm::Value* wr = builder_.CreateCall(snpFn, {bufPtr, builder_.getInt64(64), fmtGV, recv}, "ts.wr");
                                auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
                                auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
                                llvm::Value* l64 = builder_.CreateSExt(wr, i64());
                                llvm::Value* dSz = builder_.CreateAdd(l64, builder_.getInt64(1));
                                llvm::Value* dc = builder_.CreateCall(mallocFn, {dSz}, "ts.data");
                                builder_.CreateCall(memcpyFn, {dc, bufPtr, l64});
                                llvm::Value* np = builder_.CreateGEP(i8Ty, dc, l64);
                                builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), np);
                                uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
                                llvm::Value* so = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "ts.obj");
                                builder_.CreateStore(dc, builder_.CreateStructGEP(stringType(), so, 0));
                                builder_.CreateStore(l64, builder_.CreateStructGEP(stringType(), so, 1));
                                builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
                                    builder_.CreateStructGEP(stringType(), so, 2));
                                initStringRc(so);  // #422: producer's reference, rc := 1
                                return so;
                            }
                            return emitPrimitiveToString(recv, tt->primitive);
                        }
                    }
                }
            }
        }
        return lowerCall(*call);
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        return lowerNew(*ne);
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (!ix->target || ix->indices.empty() || !ix->indices[0].value)
            return nullptr;
        // CG27e (§4.6): string indexer `s[i]` → char (i32 code point).
        {
            // Pre-warm: if the target has no sema type yet (lambda param with
            // inferred type), lower it first so the type hint gets injected
            // into the sema map, then check the type.
            auto* tt = ctx_.exprType(*ix->target);
            if (!tt) {
                // Probe the type without side effects — just check the hint map.
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ix->target.get())) {
                    sema::Symbol* sym = ctx_.exprSymbol(*id);
                    auto hit = lambdaParamTypeHint_.find(sym);
                    if (hit != lambdaParamTypeHint_.end()) {
                        ctx_.setExprType(ix->target.get(), hit->second);
                        tt = hit->second;
                    }
                }
            }
            if (tt && tt->kind == sema::TypeRef::Kind::Primitive &&
                tt->primitive == sema::PrimitiveType::String) {
                llvm::Value* strPtr = lowerExpr(*ix->target);
                llvm::Value* idx = lowerExpr(*ix->indices[0].value);
                if (!strPtr || !idx) return nullptr;
                llvm::Value* idx64 = idx->getType() == i64() ? idx
                    : builder_.CreateSExt(idx, i64(), "str.idx64");
                return loadCodePoint(strPtr, idx64);
            }
        }
        // §22.10: class/record indexer property getter.
        if (auto* idxGetVal = lowerIndexerGet(*ix))
            return idxGetVal;
        // issue #388 / stdlib §4.4-§4.9: heap-collection indexer read
        // `a[i]` / `dict[key]` against the runtime backing.
        if (auto* collGetVal = lowerCollectionIndexerGet(*ix))
            return collGetVal;
        // §15.1: inline array `a[i]` — GEP the array slot and load.
        llvm::Type* ety = nullptr;
        llvm::Value* addr = lvalueAddr(*ix->target, ety);
        if (!addr || !ety) return nullptr;
        auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(ety);
        if (!arrTy) return nullptr;
        llvm::Value* idx = lowerExpr(*ix->indices[0].value);
        if (!idx) return nullptr;
        // §15.1 / §31.2: bounds-check idx against the array's compile-time
        // size; out-of-range raises IndexOutOfRangeException.
        {
            uint64_t sz = arrTy->getNumElements();
            llvm::Value* idx64 = idx->getType()->isIntegerTy(64)
                ? idx : builder_.CreateSExt(idx, i64());
            llvm::Value* szV = builder_.getInt64(sz);
            // Raise if idx < 0 (i.e., (uint64)idx >= sz when sign-extended)
            // OR idx >= sz — use an unsigned comparison covering both.
            llvm::Value* oor = builder_.CreateICmpUGE(idx64, szV, "arr.oor");
            emitGuardThrow(oor, "IndexOutOfRangeException");
        }
        llvm::Value* elemAddr = builder_.CreateGEP(
            arrTy, addr, {builder_.getInt64(0), idx});
        return builder_.CreateLoad(arrTy->getElementType(), elemAddr);
    }
    // CG34: object initializer `new C() with f := v, …`.
    if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        return lowerWith(*we);
    }
    // CG19: `is` / `is not` / `as` type tests (§12.7).
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        return lowerTypeTest(*te);
    }
    // CG28: `case` expression (§19).
    if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
        return lowerCaseExpr(*ce);
    }
    // CG60: lambda expression `(params) => body` — emitted as a
    // nested LLVM function; the lambda value is its function pointer.
    // Non-capturing only (per §A.7, captures are a later slice).
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        return lowerLambda(*lam);
    }
    // CG41: null-conditional member access `x?.M` (§7.5). Evaluate the
    // target; if null, the result is null, otherwise read the member.
    // The result is always nullable. Reference-typed storage members
    // only — value-typed results (`int?`, …) and computed-getter /
    // method `?.` are deferred.
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (!nm->target) return nullptr;
        // §7.5 / issue #416: `b?.V` on a NULLABLE RECORD. The target lowers to
        // the tagged {i8 hasValue; <record payload>} struct, not a pointer, so
        // the reference-member path below is bypassed. If hasValue is false the
        // result is null; otherwise extract the payload field and lift it to the
        // (always-nullable) result type of §7.5 — a value-typed field wraps into
        // its tagged `T?`, a reference-typed field stays a (possibly-null) ptr.
        if (auto* tt = ctx_.exprType(*nm->target)) {
            if (isNullableRecordTy(tt)) {
                llvm::Value* nv = lowerExpr(*nm->target);
                if (!nv || !nv->getType()->isStructTy()) return nullptr;
                auto* ms = storageMemberOf(ctx_.exprSymbol(e));
                if (!ms || !ms->parent) return nullptr;
                auto idxIt = fieldIndex_.find(ms);
                if (idxIt == fieldIndex_.end()) return nullptr;
                llvm::Value* hasVal = nullablePrimHasValue(nv);       // i1
                llvm::Value* payload = nullablePrimValue(nv);         // record
                llvm::Value* fieldVal =
                    builder_.CreateExtractValue(payload, {idxIt->second});
                // Lift the field value to the §7.5 (always-nullable) result. Sema
                // does not record a TypeRef for the `?.` node, so derive the form
                // from the field's own type: a reference-typed field is a nullable
                // ptr (null when the receiver is null); a field that is ALREADY a
                // nullable value (the tagged struct) passes through unwrapped but
                // forced null when the receiver is null; any other value-typed
                // field wraps into its tagged {i8 hasValue; field value} `T?`.
                const sema::TypeRef* fieldTy = recordMemberType(ms);
                if (fieldVal->getType()->isPointerTy()) {
                    // Reference-typed field → null when the receiver is null,
                    // else the field pointer.
                    return builder_.CreateSelect(
                        hasVal, fieldVal,
                        llvm::ConstantPointerNull::get(ptrTy()));
                }
                if (fieldTy && isNullableValueTy(fieldTy) &&
                    fieldVal->getType()->isStructTy()) {
                    // Field already nullable: AND its own hasValue with the
                    // receiver's so a null receiver yields null.
                    llvm::Value* innerHas = nullablePrimHasValue(fieldVal);
                    llvm::Value* combined = builder_.CreateAnd(hasVal, innerHas);
                    return builder_.CreateInsertValue(
                        fieldVal,
                        builder_.CreateZExt(combined, nullablePrimHasValTy()),
                        {0u});
                }
                // Value-typed (non-nullable) field → tagged {hasValue, fieldVal},
                // with the receiver's run-time hasValue tag.
                auto* nst = llvm::StructType::get(
                    llvm_, {nullablePrimHasValTy(), fieldVal->getType()});
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst),
                    builder_.CreateZExt(hasVal, nullablePrimHasValTy()), {0u});
                res = builder_.CreateInsertValue(res, fieldVal, {1u});
                return res;
            }
        }
        // §7.5 / FINDING [3]: the receiver is a class/string reference, so a null
        // receiver is the null pointer. Resolve the accessed member to either a
        // storage field (class receiver) or the `Length` intrinsic (string
        // receiver). The MEMBER may be value-typed (`int`, `bool`, an enum, a
        // record) — its §7.5 (always-nullable) result is the tagged
        // {i8 hasValue; T value} struct, with hasValue=false when the receiver is
        // null. Reference-typed members keep the null-pointer-as-null
        // representation (the prior path).
        llvm::Value* obj = lowerExpr(*nm->target);
        if (!obj || !obj->getType()->isPointerTy()) return nullptr;
        // Determine the accessed member's LLVM type without yet emitting the
        // access: a storage field's element type, or i32 for `string.Length`.
        auto* targetTy = ctx_.exprType(*nm->target);
        bool stringLength = nm->member == "Length" && targetTy &&
            targetTy->kind == sema::TypeRef::Kind::Primitive &&
            targetTy->primitive == sema::PrimitiveType::String;
        auto* ms = storageMemberOf(ctx_.exprSymbol(e));
        llvm::StructType* st = nullptr;
        llvm::Type* memTy = nullptr;
        unsigned memIdx = 0;
        if (stringLength) {
            memTy = i32();  // stdlib §9.6: Length: int
        } else {
            if (!ms || !ms->parent) return nullptr;
            auto idxIt = fieldIndex_.find(ms);
            if (idxIt == fieldIndex_.end()) return nullptr;
            st = classStruct(ms->parent);
            memIdx = idxIt->second;
            memTy = st->getElementType(memIdx);
        }
        // The §7.5 result form the member lifts to. A reference-typed member is a
        // (possibly-null) pointer; a value-typed member is the tagged
        // {i8 hasValue; T value} struct (StructType::get is uniqued, so building
        // it from the member's LLVM type matches the consumer's slot type). The
        // `?.` TypeRef recorded by sema (the nullable member type) tells us when
        // the member is ITSELF already nullable.
        const sema::TypeRef* resultTy = ctx_.exprType(e);
        bool refMember = memTy->isPointerTy();
        // A member that is ITSELF nullable (`int?` field) already loads as the
        // tagged {i8 hasValue; T} struct; its own LLVM type equals the result's
        // nullable struct. A NON-nullable value member (`int`, a record) loads as
        // a bare value whose LLVM type is strictly the payload, so it must be
        // wrapped. Distinguish by comparing the member's loaded type against the
        // result's tagged struct — NOT by `resultTy` alone (the §7.5-lifted
        // result is nullable for both, so it cannot tell them apart).
        bool nullableMember = !refMember && resultTy &&
            isNullableValueTy(resultTy) &&
            memTy == nullableValueStructTy(resultTy);
        llvm::Type* resTy = refMember
            ? static_cast<llvm::Type*>(ptrTy())
            : (nullableMember
                   ? memTy  // member already a {i8 hasValue; T} struct
                   : static_cast<llvm::Type*>(llvm::StructType::get(
                         llvm_, {nullablePrimHasValTy(), memTy})));
        auto* entryBB = builder_.GetInsertBlock();
        auto* accessBB =
            llvm::BasicBlock::Create(llvm_, "nullcond.access", currentFn_);
        auto* endBB =
            llvm::BasicBlock::Create(llvm_, "nullcond.end", currentFn_);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(isNull, endBB, accessBB);
        builder_.SetInsertPoint(accessBB);
        llvm::Value* val;
        if (stringLength) {
            llvm::Value* len64 = loadStringLen(obj);
            val = builder_.CreateTrunc(len64, i32(), "str.len.i32");
        } else {
            llvm::Value* addr = builder_.CreateStructGEP(st, obj, memIdx);
            val = builder_.CreateLoad(memTy, addr);
        }
        // Lift the accessed value to the §7.5 (always-nullable) result. A
        // reference member's pointer is its own null sentinel; a member that is
        // itself an `int?` passes its own hasValue through (the receiver is
        // non-null on this edge); any other value member wraps into {i8 1, val}.
        llvm::Value* accessVal;
        if (refMember || nullableMember) {
            accessVal = val;
        } else {
            accessVal = builder_.CreateInsertValue(
                llvm::UndefValue::get(resTy),
                llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
            accessVal = builder_.CreateInsertValue(accessVal, val, {1u});
        }
        auto* accessEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        // The null-receiver incoming value: a null pointer for reference members,
        // or the all-zero (hasValue=false) tagged struct for value members.
        llvm::Value* nullVal = refMember
            ? static_cast<llvm::Value*>(
                  llvm::ConstantPointerNull::get(ptrTy()))
            : static_cast<llvm::Value*>(
                  llvm::ConstantAggregateZero::get(resTy));
        auto* phi = builder_.CreatePHI(resTy, 2);
        phi->addIncoming(nullVal, entryBB);
        phi->addIncoming(accessVal, accessEnd);
        return phi;
    }
    // §3.4: static properties on primitive type names (`int.MaxValue`,
    // `double.NaN`, etc.) — the target is a primitive-name identifier.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    ma->target.get())) {
                sema::PrimitiveType pt = sema::primitiveFromName(id->name);
                if (pt != sema::PrimitiveType::None) {
                    const auto& mbr = ma->member;
                    using P = sema::PrimitiveType;
                    if (mbr == "MaxValue") {
                        switch (pt) {
                            case P::Int:  return llvm::ConstantInt::get(i32(), 0x7FFFFFFF, true);
                            case P::UInt: return llvm::ConstantInt::get(i32(), 0xFFFFFFFFu, false);
                            case P::Long: return llvm::ConstantInt::get(i64(), 0x7FFFFFFFFFFFFFFFll, true);
                            case P::ULong:return llvm::ConstantInt::get(i64(), 0xFFFFFFFFFFFFFFFFull, false);
                            case P::Short:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), 32767, true);
                            case P::UShort:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), 65535, false);
                            case P::Byte: return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 255, false);
                            case P::SByte:return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 127, true);
                            case P::Double:return llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), std::numeric_limits<double>::max());
                            case P::Float: return llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_), std::numeric_limits<float>::max());
                            default: break;
                        }
                    }
                    if (mbr == "MinValue") {
                        switch (pt) {
                            case P::Int:  return llvm::ConstantInt::get(i32(), int32_t(-2147483648ll), true);
                            case P::Long: return llvm::ConstantInt::get(i64(), int64_t(-9223372036854775808ll), true);
                            case P::Short:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), int16_t(-32768), true);
                            case P::SByte:return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), int8_t(-128), true);
                            case P::UInt: case P::ULong: case P::UShort: case P::Byte:
                                return llvm::ConstantInt::get(lowerPrimitive(pt), 0, false);
                            case P::Double: return llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), std::numeric_limits<double>::lowest());
                            case P::Float:  return llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_), std::numeric_limits<float>::lowest());
                            default: break;
                        }
                    }
                    if (mbr == "NaN" && (pt == P::Double || pt == P::Float)) {
                        llvm::Type* ft = pt == P::Double ? llvm::Type::getDoubleTy(llvm_) : llvm::Type::getFloatTy(llvm_);
                        return llvm::ConstantFP::getNaN(ft);
                    }
                    if (mbr == "PositiveInfinity" && (pt == P::Double || pt == P::Float)) {
                        llvm::Type* ft = pt == P::Double ? llvm::Type::getDoubleTy(llvm_) : llvm::Type::getFloatTy(llvm_);
                        return llvm::ConstantFP::getInfinity(ft, false);
                    }
                    if (mbr == "NegativeInfinity" && (pt == P::Double || pt == P::Float)) {
                        llvm::Type* ft = pt == P::Double ? llvm::Type::getDoubleTy(llvm_) : llvm::Type::getFloatTy(llvm_);
                        return llvm::ConstantFP::getInfinity(ft, true);
                    }
                    if (mbr == "Epsilon" && (pt == P::Double || pt == P::Float)) {
                        // Smallest positive value: 5e-324 for double, ~1.4e-45 for float.
                        return pt == P::Double
                            ? llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_),
                                std::numeric_limits<double>::denorm_min())
                            : llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_),
                                std::numeric_limits<float>::denorm_min());
                    }
                }
                // §2.3: built-in class constant properties (Math.PI, Math.E, etc.)
                // These are MemberAccess on a CLASS name (not a primitive type).
                if (pt == sema::PrimitiveType::None) {
                    // Not a primitive keyword — check for built-in class names.
                }
            } else {
                // Not a primitive keyword identifier — check for built-in class names.
            }
        }
    }
    // §2.3: Math.PI / Math.E + Environment.NewLine / TickCount etc.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (id->name == "Math") {
                    // §2.2: spec constants Pi / E / Tau. `PI` (uppercase) is a
                    // legacy spelling kept for the existing intrinsic test.
                    if (ma->member == "Pi" || ma->member == "PI")
                        return llvm::ConstantFP::get(dbl, 3.14159265358979323846);
                    if (ma->member == "E")  return llvm::ConstantFP::get(dbl, 2.71828182845904523536);
                    if (ma->member == "Tau") return llvm::ConstantFP::get(dbl, 6.28318530717958647692);
                }
                if (id->name == "Environment") {
                    // §2.3: Environment static properties.
                    if (ma->member == "NewLine") {
                        // Platform newline: "\n" on Unix, "\r\n" on Windows.
                        // Emit a global string constant for "\n" (Unix default).
                        return lowerStringLiteral("\"\\n\"", false);
                    }
                    if (ma->member == "TickCount") {
                        // Milliseconds since process start (via clock_gettime).
                        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                        auto* clockFn = module_->getFunction("clock");
                        if (!clockFn) {
                            auto* ft = llvm::FunctionType::get(i64(), {}, false);
                            clockFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "clock", module_.get());
                        }
                        llvm::Value* clk = builder_.CreateCall(clockFn, {}, "clock.ret");
                        // CLOCKS_PER_SEC ≈ 1000000 on Linux; scale to ms.
                        llvm::Value* ms = builder_.CreateSDiv(
                            clk, builder_.getInt64(1000), "tick.ms");
                        return builder_.CreateTrunc(ms, i32(), "tick.i32");
                    }
                    if (ma->member == "OSVersion") {
                        return lowerStringLiteral("\"Linux\"", false);
                    }
                    if (ma->member == "MachineName") {
                        return lowerStringLiteral("\"localhost\"", false);
                    }
                    if (ma->member == "ProcessId") {
                        auto* getpidFn = module_->getFunction("getpid");
                        if (!getpidFn) {
                            auto* ft = llvm::FunctionType::get(i32(), {}, false);
                            getpidFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "getpid", module_.get());
                        }
                        return builder_.CreateCall(getpidFn, {}, "pid.ret");
                    }
                    if (ma->member == "ProcessorCount") {
                        // nproc via sysconf(_SC_NPROCESSORS_ONLN) = sysconf(84)
                        auto* scFn = module_->getFunction("sysconf");
                        if (!scFn) {
                            auto* ft = llvm::FunctionType::get(i64(), {i32()}, false);
                            scFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "sysconf", module_.get());
                        }
                        llvm::Value* n = builder_.CreateCall(scFn, {builder_.getInt32(84)});
                        return builder_.CreateTrunc(n, i32(), "nproc");
                    }
                    if (ma->member == "CurrentDirectory") {
                        // getcwd(NULL, 0) on Linux allocates a buffer; wrap as bz.string
                        auto* getcwdFn = module_->getFunction("getcwd");
                        if (!getcwdFn) {
                            auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i64()}, false);
                            getcwdFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "getcwd", module_.get());
                        }
                        auto* strlenFn = module_->getFunction("strlen");
                        if (!strlenFn) {
                            auto* ft2 = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                            strlenFn = llvm::Function::Create(ft2, llvm::Function::ExternalLinkage, "strlen", module_.get());
                        }
                        llvm::Value* raw = builder_.CreateCall(getcwdFn, {
                            llvm::ConstantPointerNull::get(ptrTy()), builder_.getInt64(0)});
                        llvm::Value* len = builder_.CreateCall(strlenFn, {raw});
                        // Build bz.string from malloc'd C string (raw, len)
                        auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
                        uint64_t strSz = module_->getDataLayout().getTypeAllocSize(stringType());
                        llvm::Value* strObj = builder_.CreateCall(callocFn2,
                            {builder_.getInt64(1), builder_.getInt64(strSz)});
                        builder_.CreateStore(raw, builder_.CreateStructGEP(stringType(), strObj, 0));
                        builder_.CreateStore(len, builder_.CreateStructGEP(stringType(), strObj, 1));
                        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
                            builder_.CreateStructGEP(stringType(), strObj, 2));
                            initStringRc(strObj);  // #422: producer's reference, rc := 1
                        return strObj;
                    }
                }
                // §2.5: DateTime static properties accessed as property reads
                // (without parentheses): DateTime.Now, DateTime.UtcNow, etc.
                // Also handles TimeSpan.Zero and TimeSpan.FromX() zero args.
                if (id->name == "DateTime") {
                    const auto& mbr = ma->member;
                    if (mbr == "Now") {
                        auto* fn = declareC("bz_datetime_now_ticks", i64(), {});
                        return builder_.CreateCall(fn, {}, "dt.now");
                    }
                    if (mbr == "UtcNow") {
                        auto* fn = declareC("bz_datetime_utcnow_ticks", i64(), {});
                        return builder_.CreateCall(fn, {}, "dt.utcnow");
                    }
                    if (mbr == "Today") {
                        auto* fn = declareC("bz_datetime_now_ticks", i64(), {});
                        llvm::Value* t = builder_.CreateCall(fn, {}, "dt.today.ticks");
                        llvm::Value* tpd = builder_.getInt64(864000000000LL);
                        llvm::Value* d = builder_.CreateSDiv(t, tpd);
                        return builder_.CreateMul(d, tpd, "dt.today");
                    }
                    if (mbr == "MinValue")
                        return builder_.getInt64(0LL);
                    if (mbr == "MaxValue")
                        return builder_.getInt64(3155378975999999999LL);
                }
                if (id->name == "TimeSpan") {
                    const auto& mbr = ma->member;
                    if (mbr == "Zero")
                        return builder_.getInt64(0LL);
                    // TimeSpan.FromX static factories are handled in
                    // tryLowerBuiltinClassCall (CallExpr path); but if accessed
                    // without () (not valid Bezel for factories), return nullptr.
                }
                if (id->name == "int") {
                    if (ma->member == "MaxValue")
                        return llvm::ConstantInt::getSigned(i32(), 2147483647);
                    if (ma->member == "MinValue")
                        return llvm::ConstantInt::getSigned(i32(), -2147483648LL);
                }
                if (id->name == "long") {
                    if (ma->member == "MaxValue")
                        return llvm::ConstantInt::getSigned(i64(), 9223372036854775807LL);
                    if (ma->member == "MinValue")
                        return llvm::ConstantInt::getSigned(i64(), (int64_t)(-9223372036854775807LL - 1));
                }
                if (id->name == "double") {
                    if (ma->member == "MaxValue")
                        return llvm::ConstantFP::get(dbl, 1.7976931348623157e308);
                    if (ma->member == "MinValue")
                        return llvm::ConstantFP::get(dbl, 2.2250738585072014e-308);
                    if (ma->member == "Epsilon")
                        return llvm::ConstantFP::get(dbl, 5e-324);
                    if (ma->member == "PositiveInfinity")
                        return llvm::ConstantFP::getInfinity(dbl, false);
                    if (ma->member == "NegativeInfinity")
                        return llvm::ConstantFP::getInfinity(dbl, true);
                    if (ma->member == "NaN")
                        return llvm::ConstantFP::getNaN(dbl);
                }
            }
        }
    }
    // CG27a (§4.6): intrinsic string member access (Length, etc.).
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            if (tt && tt->kind == sema::TypeRef::Kind::Primitive &&
                tt->primitive == sema::PrimitiveType::String) {
                llvm::Value* strPtr = lowerExpr(*ma->target);
                if (!strPtr) return nullptr;
                if (ma->member == "Length") {
                    // stdlib §9.6: Length: int — number of char code points.
                    llvm::Value* len64 = loadStringLen(strPtr);
                    return builder_.CreateTrunc(len64, i32(), "str.len.i32");
                }
                // Other string members deferred — sema would have flagged
                // any unknown member already.
                return nullptr;
            }
            // Fallback for cases where sema type is truly unknown (e.g. e.Message
            // on a built-in exception — sema doesn't know Message returns
            // string). Only apply when the target has NO declaration (built-in
            // exceptions) and NOT a Named type with a known user declaration
            // (which might have a computed 'Length' property of its own).
            // Exclude StringBuilder from the string-fallback (it has its own
            // Length handler that sums part lengths).
            auto notStrBld = [&]() -> bool {
                return !(tt && tt->kind == sema::TypeRef::Kind::Named &&
                         !tt->declaration && tt->astNode && tt->astNode->name &&
                         !tt->astNode->name->parts.empty() &&
                         tt->astNode->name->parts[0] == "StringBuilder");
            };
            if (ma->member == "Length" && notStrBld() &&
                (!tt ||
                 (tt->kind == sema::TypeRef::Kind::Named &&
                  !tt->declaration))) {
                // Only intercept if it's not a computed property on the target.
                if (!computedGetterOf(ctx_.exprSymbol(e)) &&
                    !blockGetterOf(ctx_.exprSymbol(e))) {
                    llvm::Value* strPtr = lowerExpr(*ma->target);
                    if (strPtr && strPtr->getType()->isPointerTy()) {
                        // If the value is a bz_string struct pointer, load Length.
                        llvm::Value* len64 = loadStringLen(strPtr);
                        return builder_.CreateTrunc(len64, i32(), "str.len.i32");
                    }
                }
            }
        }
    }
    // §6.1 / §30.8: CancellationToken and CancellationTokenSource property reads
    // (accessed WITHOUT call parentheses — `tok.IsCancellationRequested` etc.).
    // Mirrors the pattern used for string.Length above: recognise the builtin
    // type name from the receiver's exprType astNode name part.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            std::string_view cn = builtinTypeName(tt);
            // CancellationToken property reads.
            if (cn == "CancellationToken" &&
                ma->member == "IsCancellationRequested") {
                llvm::Value* tok = lowerExpr(*ma->target);
                if (!tok) return builder_.getFalse();
                auto* fn = declareC("bz_ctoken_is_cancelled", i32(), {ptrTy()});
                llvm::Value* flag = builder_.CreateCall(fn, {tok}, "ct.iscancelled");
                return builder_.CreateICmpNE(flag, builder_.getInt32(0), "ct.icr.b");
            }
            // CancellationTokenSource property reads.
            if (cn == "CancellationTokenSource") {
                llvm::Value* cts = lowerExpr(*ma->target);
                if (!cts) return nullptr;
                if (ma->member == "IsCancellationRequested") {
                    auto* fn = declareC("bz_cts_is_cancelled", i32(), {ptrTy()});
                    llvm::Value* flag = builder_.CreateCall(fn, {cts}, "cts.iscancelled");
                    return builder_.CreateICmpNE(flag, builder_.getInt32(0), "cts.icr.b");
                }
                if (ma->member == "Token") {
                    auto* fn = declareC("bz_cts_token", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {cts}, "cts.token.prop");
                }
            }
            // CancellationToken.None (static property, target is type-name identifier).
            if (auto* id2 = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
                if (id2->name == "CancellationToken" && ma->member == "None") {
                    return llvm::ConstantPointerNull::get(ptrTy());
                }
                // stdlib §2.6: Guid.Empty — the all-zero Guid (static readonly property).
                if (id2->name == "Guid" && ma->member == "Empty") {
                    return llvm::ConstantAggregateZero::get(guidStructType());
                }
            }
        }
    }
    // §2.5: DateTime and TimeSpan instance property reads.
    // DateTime/TimeSpan are scalar i64 tick values; properties are computed
    // by calling the appropriate bz_datetime_* / bz_timespan_* runtime helpers.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            std::string_view cn = builtinTypeName(tt);
            if (cn == "DateTime") {
                llvm::Value* ticks = lowerExpr(*ma->target);
                if (!ticks) return nullptr;
                const auto& mbr = ma->member;
                // Integer (i32) component properties.
                const char* fnName = nullptr;
                if      (mbr == "Year")        fnName = "bz_datetime_year";
                else if (mbr == "Month")       fnName = "bz_datetime_month";
                else if (mbr == "Day")         fnName = "bz_datetime_day";
                else if (mbr == "Hour")        fnName = "bz_datetime_hour";
                else if (mbr == "Minute")      fnName = "bz_datetime_minute";
                else if (mbr == "Second")      fnName = "bz_datetime_second";
                else if (mbr == "Millisecond") fnName = "bz_datetime_millisecond";
                else if (mbr == "DayOfWeek")   fnName = "bz_datetime_day_of_week";
                else if (mbr == "DayOfYear")   fnName = "bz_datetime_day_of_year";
                if (fnName) {
                    auto* fn = declareC(fnName, i32(), {i64()});
                    return builder_.CreateCall(fn, {ticks}, llvm::Twine(mbr));
                }
                // Ticks property — return the raw i64.
                if (mbr == "Ticks") return ticks;
            }
            if (cn == "TimeSpan") {
                llvm::Value* ticks = lowerExpr(*ma->target);
                if (!ticks) return nullptr;
                const auto& mbr = ma->member;
                // Total* properties return double.
                if (mbr == "TotalDays") {
                    auto* fn = declareC("bz_timespan_total_days",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totaldays");
                }
                if (mbr == "TotalHours") {
                    auto* fn = declareC("bz_timespan_total_hours",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totalhours");
                }
                if (mbr == "TotalMinutes") {
                    auto* fn = declareC("bz_timespan_total_minutes",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totalmins");
                }
                if (mbr == "TotalSeconds") {
                    auto* fn = declareC("bz_timespan_total_seconds",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totalsecs");
                }
                if (mbr == "TotalMilliseconds") {
                    auto* fn = declareC("bz_timespan_total_milliseconds",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totalms");
                }
                // Component (int) properties — decompose via bz_timespan_components.
                // Each allocates output slots, calls the helper, returns one slot.
                auto emitComponent = [&](int slot) -> llvm::Value* {
                    llvm::Value* dS  = entryAlloca(i32());
                    llvm::Value* hS  = entryAlloca(i32());
                    llvm::Value* miS = entryAlloca(i32());
                    llvm::Value* sS  = entryAlloca(i32());
                    llvm::Value* msS = entryAlloca(i32());
                    auto* fn = declareC("bz_timespan_components",
                        llvm::Type::getVoidTy(llvm_),
                        {i64(), ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                    builder_.CreateCall(fn, {ticks, dS, hS, miS, sS, msS});
                    llvm::Value* slots[] = {dS, hS, miS, sS, msS};
                    return builder_.CreateLoad(i32(), slots[slot]);
                };
                if (mbr == "Days")         return emitComponent(0);
                if (mbr == "Hours")        return emitComponent(1);
                if (mbr == "Minutes")      return emitComponent(2);
                if (mbr == "Seconds")      return emitComponent(3);
                if (mbr == "Milliseconds") return emitComponent(4);
                // Ticks property — raw i64.
                if (mbr == "Ticks") return ticks;
            }
        }
    }
    // §15.1: inline array `a.Length` — compile-time constant equal to N.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Length") {
            auto* tt = ctx_.exprType(*ma->target);
            if (tt && tt->kind == sema::TypeRef::Kind::InlineArray) {
                // The array size is encoded in tt->arraySize (IntLiteralExpr).
                // Evaluate it as a compile-time constant.
                if (auto* slit = dynamic_cast<const ast::IntLiteralExpr*>(tt->arraySize)) {
                    int64_t sz = (int64_t)parseIntLiteral(slit->text);
                    return llvm::ConstantInt::get(i32(), sz, true);
                }
            }
            // Also handle the case where the target is an inline-array lvalue
            // and we can get the array type from the LLVM type.
            llvm::Type* elemTy = nullptr;
            if (llvm::Value* addr = lvalueAddr(*ma->target, elemTy)) {
                if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(elemTy)) {
                    return llvm::ConstantInt::get(i32(), arrTy->getNumElements(), true);
                }
            }
        }
    }
    // §3: Array<T>.Length / .Count — runtime bz_array_len.
    // StringBuilder.Length — sum of part lengths (total chars).
    // Dictionary<K,V>.Count — runtime bz_dict_count.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && (ma->member == "Length" || ma->member == "Count")) {
            auto* tt = ctx_.exprType(*ma->target);
            // StringBuilder.Length: sum of all part lengths.
            bool isSB = (tt && tt->kind == sema::TypeRef::Kind::Named &&
                         !tt->declaration && tt->astNode && tt->astNode->name &&
                         !tt->astNode->name->parts.empty() &&
                         tt->astNode->name->parts[0] == "StringBuilder");
            if (isSB) {
                llvm::Value* sb = lowerExpr(*ma->target);
                if (sb) {
                    constexpr int64_t SB_ESZ2 = 8;
                    auto* lenFn2 = declareC("bz_array_len", i64(), {ptrTy()});
                    auto* ptrFn2 = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
                    llvm::Value* nP = builder_.CreateCall(lenFn2, {sb});
                    auto* sumSlot = entryAlloca(i64());
                    auto* idxSlot = entryAlloca(i64());
                    builder_.CreateStore(builder_.getInt64(0), sumSlot);
                    builder_.CreateStore(builder_.getInt64(0), idxSlot);
                    auto* cBB = llvm::BasicBlock::Create(llvm_, "sbl.c", currentFn_);
                    auto* bBB = llvm::BasicBlock::Create(llvm_, "sbl.b", currentFn_);
                    auto* eBB = llvm::BasicBlock::Create(llvm_, "sbl.e", currentFn_);
                    builder_.CreateBr(cBB);
                    builder_.SetInsertPoint(cBB);
                    llvm::Value* si = builder_.CreateLoad(i64(), idxSlot);
                    builder_.CreateCondBr(builder_.CreateICmpSLT(si, nP), bBB, eBB);
                    builder_.SetInsertPoint(bBB);
                    llvm::Value* ep = builder_.CreateCall(ptrFn2, {sb, si, builder_.getInt64(SB_ESZ2)});
                    llvm::Value* part = builder_.CreateLoad(ptrTy(), ep);
                    llvm::Value* plen = loadStringLen(part);
                    llvm::Value* cur = builder_.CreateLoad(i64(), sumSlot);
                    builder_.CreateStore(builder_.CreateAdd(cur, plen), sumSlot);
                    builder_.CreateStore(builder_.CreateAdd(si, builder_.getInt64(1)), idxSlot);
                    builder_.CreateBr(cBB);
                    builder_.SetInsertPoint(eBB);
                    return builder_.CreateTrunc(builder_.CreateLoad(i64(), sumSlot), i32());
                }
            }
            // per §4.6/§4.7: LinkedList<T>/OwnedLinkedList<T>.Count via bz_llist_count.
            if (isLinkedListTypeName(tt)) {
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj) {
                    auto* fn = declareC("bz_llist_count", i64(), {ptrTy()});
                    llvm::Value* cnt64 = builder_.CreateCall(fn, {obj});
                    return builder_.CreateTrunc(cnt64, i32(), "ll.cnt");
                }
            }
            if (isCollectionTypeName(tt)) {
                // issues #343 / D8 / §4.10: a Set is dict-backed (records,
                // primitives, AND strings — see lowerNew), so its Count comes
                // from bz_dict_count, not the array length.
                bool isSetName = tt->astNode && tt->astNode->name &&
                                 !tt->astNode->name->parts.empty() &&
                                 (tt->astNode->name->parts[0] == "Set" ||
                                  tt->astNode->name->parts[0] == "OwnedSet");
                bool dictBacked = isSetName &&
                    genericBackingType(tt, 0, /*isSet=*/true, nullptr) != nullptr;
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj) {
                    auto* fn = dictBacked
                        ? declareC("bz_dict_count", i64(), {ptrTy()})
                        : declareC("bz_array_len", i64(), {ptrTy()});
                    llvm::Value* len64 = builder_.CreateCall(fn, {obj});
                    return builder_.CreateTrunc(len64, i32(), "col.len");
                }
            }
            if (isDictTypeName(tt)) {
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj) {
                    auto* fn = declareC("bz_dict_count", i64(), {ptrTy()});
                    llvm::Value* cnt64 = builder_.CreateCall(fn, {obj});
                    return builder_.CreateTrunc(cnt64, i32(), "dict.cnt");
                }
            }
        }
    }
    // per §4.6/§4.7: LinkedList<T>/OwnedLinkedList<T> node-returning properties
    // (FirstNode, LastNode, First, Last) and LinkedListNode<T> accessor
    // properties (Value, Next, Previous).
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            bool isLL = isLinkedListTypeName(tt);
            // LinkedList<T> node properties.
            if (isLL && (ma->member == "FirstNode" || ma->member == "LastNode" ||
                         ma->member == "First" || ma->member == "Last")) {
                llvm::Value* list = lowerExpr(*ma->target);
                if (!list) return nullptr;
                int64_t esz = collectionElemSize(tt);
                llvm::Type* elemLLTy = i32();
                if (tt && !tt->genericArgs.empty() && tt->genericArgs[0])
                    elemLLTy = lowerTypeRef(tt->genericArgs[0]);
                if (ma->member == "FirstNode" || ma->member == "LastNode") {
                    const char* fn = ma->member == "FirstNode"
                        ? "bz_llist_first_node" : "bz_llist_last_node";
                    auto* f = declareC(fn, ptrTy(), {ptrTy()});
                    return builder_.CreateCall(f, {list}, "ll.nodeptr");
                }
                // First / Last: return the value of the first/last node (T).
                // If list is empty this is undefined — same as Array.First() on empty.
                {
                    const char* fn = ma->member == "First"
                        ? "bz_llist_first_node" : "bz_llist_last_node";
                    auto* nodeF = declareC(fn, ptrTy(), {ptrTy()});
                    llvm::Value* nodePtr = builder_.CreateCall(nodeF, {list}, "ll.fnode");
                    auto* valF = declareC("bz_llist_node_value_ptr", ptrTy(), {ptrTy()});
                    llvm::Value* valPtr = builder_.CreateCall(valF, {nodePtr}, "ll.vptr");
                    return builder_.CreateLoad(elemLLTy, valPtr, "ll.val");
                }
            }
            // LinkedListNode<T> accessor properties.
            bool isNode = (tt && tt->kind == sema::TypeRef::Kind::Named &&
                           !tt->declaration && tt->astNode && tt->astNode->name &&
                           !tt->astNode->name->parts.empty() &&
                           tt->astNode->name->parts[0] == "LinkedListNode");
            if (isNode) {
                llvm::Value* nodePtr = lowerExpr(*ma->target);
                if (!nodePtr) return nullptr;
                if (ma->member == "Value") {
                    // elem type is the first generic arg.
                    llvm::Type* elemLLTy2 = ptrTy();
                    if (tt && !tt->genericArgs.empty() && tt->genericArgs[0])
                        elemLLTy2 = lowerTypeRef(tt->genericArgs[0]);
                    auto* valF = declareC("bz_llist_node_value_ptr", ptrTy(), {ptrTy()});
                    llvm::Value* vp = builder_.CreateCall(valF, {nodePtr}, "nd.vptr");
                    return builder_.CreateLoad(elemLLTy2, vp, "nd.val");
                }
                if (ma->member == "Next") {
                    auto* f = declareC("bz_llist_node_next", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(f, {nodePtr}, "nd.next");
                }
                if (ma->member == "Previous") {
                    auto* f = declareC("bz_llist_node_prev", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(f, {nodePtr}, "nd.prev");
                }
            }
        }
    }
    // §3: Dictionary<K,V>.Keys — property returning Array<string> of all keys.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Keys") {
            auto* tt = ctx_.exprType(*ma->target);
            if (isDictTypeName(tt)) {
                llvm::Value* dictObj = lowerExpr(*ma->target);
                if (!dictObj) return nullptr;
                auto* keysOutSlot = entryAlloca(ptrTy());
                auto* lensOutSlot = entryAlloca(ptrTy());
                auto* cntSlot2 = entryAlloca(i64());
                auto* keysFn2 = declareC("bz_dict_keys_str", llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(keysFn2, {dictObj, keysOutSlot, lensOutSlot, cntSlot2});
                llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);
                llvm::Value* lensArr = builder_.CreateLoad(ptrTy(), lensOutSlot);
                llvm::Value* cnt2 = builder_.CreateLoad(i64(), cntSlot2);
                auto* allocFn2 = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                llvm::Value* result2 = builder_.CreateCall(allocFn2,
                    {builder_.getInt64(8), cnt2});
                auto* addFn2 = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                        {ptrTy(), ptrTy(), i64()});
                auto* mallocFn2 = declareC("malloc", ptrTy(), {i64()});
                auto* memcpyFn2 = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
                auto* callocFn3 = declareC("calloc", ptrTy(), {i64(), i64()});
                auto* i8Ty2 = llvm::Type::getInt8Ty(llvm_);
                uint64_t strSz2 = module_->getDataLayout().getTypeAllocSize(stringType());
                auto* idxSlot3 = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), idxSlot3);
                auto* condBB2b = llvm::BasicBlock::Create(llvm_, "ks2.c", currentFn_);
                auto* bodyBB2b = llvm::BasicBlock::Create(llvm_, "ks2.b", currentFn_);
                auto* endBB2b  = llvm::BasicBlock::Create(llvm_, "ks2.e", currentFn_);
                builder_.CreateBr(condBB2b);
                builder_.SetInsertPoint(condBB2b);
                llvm::Value* idx3 = builder_.CreateLoad(i64(), idxSlot3);
                builder_.CreateCondBr(builder_.CreateICmpSLT(idx3, cnt2), bodyBB2b, endBB2b);
                builder_.SetInsertPoint(bodyBB2b);
                llvm::Value* kpPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idx3);
                llvm::Value* kp2 = builder_.CreateLoad(ptrTy(), kpPtr2);
                llvm::Value* lpPtr2 = builder_.CreateGEP(i64(), lensArr, idx3);
                llvm::Value* kl3 = builder_.CreateLoad(i64(), lpPtr2);
                llvm::Value* dataSz3 = builder_.CreateAdd(kl3, builder_.getInt64(1));
                llvm::Value* dataCopy3 = builder_.CreateCall(mallocFn2, {dataSz3});
                builder_.CreateCall(memcpyFn2, {dataCopy3, kp2, kl3});
                llvm::Value* nulPtr3 = builder_.CreateGEP(i8Ty2, dataCopy3, kl3);
                builder_.CreateStore(llvm::ConstantInt::get(i8Ty2, 0), nulPtr3);
                llvm::Value* strObj3 = builder_.CreateCall(callocFn3,
                    {builder_.getInt64(1), builder_.getInt64(strSz2)});
                builder_.CreateStore(dataCopy3, builder_.CreateStructGEP(stringType(), strObj3, 0));
                builder_.CreateStore(kl3, builder_.CreateStructGEP(stringType(), strObj3, 1));
                builder_.CreateStore(llvm::ConstantInt::get(i8Ty2, 1),
                    builder_.CreateStructGEP(stringType(), strObj3, 2));
                    initStringRc(strObj3);  // #422: producer's reference, rc := 1
                auto* slotK2 = entryAlloca(ptrTy());
                builder_.CreateStore(strObj3, slotK2);
                builder_.CreateCall(addFn2, {result2, slotK2, builder_.getInt64(8)});
                builder_.CreateStore(builder_.CreateAdd(idx3, builder_.getInt64(1)), idxSlot3);
                builder_.CreateBr(condBB2b);
                builder_.SetInsertPoint(endBB2b);
                return result2;
            }
        }
    }
    // CG22+: Exception.Message — built-in exceptions store a message
    // string pointer at byte offset 8 (slot 1) of the exception object.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Message") {
            auto* tt = ctx_.exprType(*ma->target);
            if (tt && isBuiltinExceptionType(tt)) {
                llvm::Value* excObj = lowerExpr(*ma->target);
                if (!excObj) return nullptr;
                // slot 1 = message string ptr (at byte offset 8)
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* msgSlot = builder_.CreateGEP(
                    i8Ty, excObj, builder_.getInt64(8));
                llvm::Value* msgPtr = builder_.CreateLoad(ptrTy(), msgSlot);
                // If null (no message given), return empty string constant.
                auto* isNull = builder_.CreateICmpEQ(
                    msgPtr,
                    llvm::ConstantPointerNull::get(ptrTy()));
                llvm::Value* emptyStr = lowerStringLiteral("\"\"", false);
                return builder_.CreateSelect(isNull, emptyStr, msgPtr);
            }
        }
    }
    // CG33b: a computed (arrow-getter) property read `obj.P` evaluates
    // the getter with `this` = obj.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (const ast::Expr* getter = computedGetterOf(ctx_.exprSymbol(*ma))) {
            if (!ma->target) return nullptr;
            // CG33b: for static properties (accessed via type name), the
            // receiver is a class type identifier, not an instance — skip
            // the obj/currentThis_ setup and evaluate the body directly.
            auto* propSym = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
            bool isStaticProp = propSym && !propSym->isIndexer &&
                std::any_of(propSym->modifiers.begin(),
                            propSym->modifiers.end(),
                            [](const auto& m){ return m == "static"; });
            if (isStaticProp) {
                // Static property: no 'this', just evaluate the getter body.
                return lowerExpr(*getter);
            }
            llvm::Value* obj = lowerExpr(*ma->target);
            if (!obj) return nullptr;
            // issue #347 / §9.2: a record is a by-value struct, not a heap
            // pointer. The getter body reads its fields against `this` via a
            // StructGEP, which requires a pointer base; binding a struct
            // *value* as `this` produces malformed IR (GEP on a non-pointer)
            // that crashes the backend. When the receiver is a record value
            // (not already a pointer), spill it to a stack slot and bind that
            // slot's address as `this` — mirroring the record field-access
            // path in fieldAddr (CG29/#254) and the §9.2 record-storage
            // access used by the synthesized ToString.
            if (!obj->getType()->isPointerTy()) {
                llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                builder_.CreateStore(obj, tmp);
                obj = tmp;
            }
            // CG-generic: set up genericSubst_ for the receiver's type so
            // that fields accessed inside the getter use the right struct.
            auto savedSubst = genericSubst_;
            if (auto* sym = ctx_.exprSymbol(*ma)) {
                if (auto* ps = dynamic_cast<sema::PropertySymbol*>(sym)) {
                    if (ps->parent && ps->parent->decl) {
                        sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                        if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                            recvTy->declaration == ps->parent &&
                            !recvTy->genericArgs.empty()) {
                            auto* td = dynamic_cast<const ast::TypeDecl*>(ps->parent->decl);
                            if (td && td->genericParams.size() == recvTy->genericArgs.size()) {
                                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                                for (std::size_t gi = 0; gi < td->genericParams.size(); ++gi) {
                                    llvm::Type* concrTy = lowerTypeRef(recvTy->genericArgs[gi]);
                                    newSubst[td->genericParams[gi].name] = concrTy;
                                }
                                // Check unresolved (i32 fallback) → use localGenericSubst_
                                bool hasUnresolved = false;
                                for (auto& [n, ty] : newSubst)
                                    if (ty == i32()) { hasUnresolved = true; break; }
                                if (hasUnresolved) {
                                    sema::Symbol* recvSym = ctx_.exprSymbol(*ma->target);
                                    const ast::Node* decl = nullptr;
                                    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(recvSym))
                                        decl = loc->decl;
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
                }
            }
            llvm::Value* saved = currentThis_;
            currentThis_ = obj;
            llvm::Value* v = lowerExpr(*getter);
            currentThis_ = saved;
            genericSubst_ = std::move(savedSubst);
            return v;
        }
        // CG33b: block-form getter (`get => do … end;`) — emit as a
        // standalone function so that `return` inside the body terminates
        // only the getter, not the enclosing method (§22.9).
        if (const ast::PropertyAccessor* acc =
                blockGetterOf(ctx_.exprSymbol(*ma))) {
            if (!ma->target) return nullptr;
            llvm::Value* obj = lowerExpr(*ma->target);
            if (!obj) return nullptr;
            // issue #347 / §9.2: the block getter is a function taking a
            // `this` pointer; a record receiver is a by-value struct, so spill
            // it to a stack slot and pass that slot's address (see the
            // expression-getter path above for the full rationale).
            if (!obj->getType()->isPointerTy()) {
                llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                builder_.CreateStore(obj, tmp);
                obj = tmp;
            }
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(
                dynamic_cast<sema::PropertySymbol*>(
                    ctx_.exprSymbol(*ma))->decl);
            llvm::Function* getterFn = ensurePropertyBlockGetter(pd, acc);
            llvm::Value* res = builder_.CreateCall(getterFn->getFunctionType(),
                                                   getterFn, {obj});
            // §31.1/§31.2 (FINDING [4]): a block-form getter is a standalone
            // function; a `throw` inside it (the §23.3 idiom) leaves the
            // pending-exception global set and returns. The caller must test
            // that flag and route to the enclosing unwind target — exactly as
            // an ordinary method call does (emitMethodCall → emitPendingCheck).
            // Without this the throw is swallowed and `res` is a garbage value.
            emitPendingCheck();
            return res;
        }
    }
    // CG8: field read `obj.f`.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        llvm::Type* elemTy = nullptr;
        if (llvm::Value* addr = fieldAddr(*ma, elemTy)) {
            return builder_.CreateLoad(elemTy, addr);
        }
        // #213: a qualified static-field read `Type.Count` — static
        // fields have no instance slot, so fieldAddr misses; read the
        // backing global directly.
        if (llvm::GlobalVariable* gv = staticFieldGlobal(e)) {
            return builder_.CreateLoad(gv->getValueType(), gv);
        }
    }
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        // Inject lambda-param type hints so type-gated branches below
        // (string subscript, member access) see the correct type.
        if (!ctx_.exprType(e)) {
            sema::Symbol* sym = ctx_.exprSymbol(e);
            auto hit = lambdaParamTypeHint_.find(sym);
            if (hit != lambdaParamTypeHint_.end())
                ctx_.setExprType(&e, hit->second);
        }
        // CG9: `this` (and `base`, same instance) is the receiver ptr.
        if (id->name == "this" || id->name == "base") return currentThis_;
        // CG33b: a bare computed-property read evaluates its getter
        // against the current `this`.
        if (const ast::Expr* getter = computedGetterOf(ctx_.exprSymbol(e)))
            return lowerExpr(*getter);
        // CG33b: block-form getter (`get => do … end;`) accessed bare.
        if (const ast::PropertyAccessor* acc = blockGetterOf(ctx_.exprSymbol(e))) {
            auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(e));
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            llvm::Function* fn = ensurePropertyBlockGetter(pd, acc);
            llvm::Value* res =
                builder_.CreateCall(fn->getFunctionType(), fn, {currentThis_});
            // §31.1/§31.2 (FINDING [4]): see the qualified block-getter read
            // above — a throw inside the standalone getter must propagate.
            emitPendingCheck();
            return res;
        }
        // CG12: a bare identifier resolving to a static field reads
        // its backing global.
        if (llvm::GlobalVariable* gv = staticFieldGlobal(e)) {
            return builder_.CreateLoad(gv->getValueType(), gv);
        }
        // CG9: a bare identifier resolving to an instance field is an
        // implicit-`this` field read.
        llvm::Type* elemTy = nullptr;
        if (llvm::Value* addr = implicitFieldAddr(e, elemTy)) {
            return builder_.CreateLoad(elemTy, addr);
        }
        // CG50: a ref/out parameter — load through the incoming pointer.
        llvm::Type* refElem = nullptr;
        if (llvm::Value* a = refParamAddr(e, refElem)) {
            return builder_.CreateLoad(refElem, a);
        }
        // CG3: a reference to a local variable loads from its slot.
        // §27.5: a captured ("boxed") local reads through its shared cell.
        if (llvm::AllocaInst* slot = localSlot(e)) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(slot, ety);
            return builder_.CreateLoad(ety, addr);
        }
        // CG60: a bare identifier resolving to a static method —
        // produce a closure struct { thunk_ptr, null_env } so it can
        // be passed as a function-typed value (§A.7 / §17.14).
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(e))) {
            if (isStaticMethod(ms)) return makeMethodThunk(ms, nullptr);
        }
    }
    // CG60: `Class.StaticMethod` or `obj.InstanceMethod` used as a
    // value (not called). Produce a closure struct for it.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(e))) {
            llvm::Value* recv = nullptr;
            if (!isStaticMethod(ms) && ma->target)
                recv = lowerExpr(*ma->target);
            return makeMethodThunk(ms, recv);
        }
    }
    return nullptr;
}


llvm::Value* Emitter::lowerBinary(const ast::BinaryExpr& bin) {
    if (!bin.left || !bin.right) return nullptr;
    // CG: short-circuit `and`/`or` (§17) — the right operand is
    // evaluated only when needed, so a side-effecting or throwing RHS
    // is not run when the result is already determined.
    if (bin.op == "and" || bin.op == "or")
        return lowerShortCircuit(bin);
    // CG39: null-coalescing `??` (§17.1 L12) short-circuits — the right
    // operand is evaluated only when the left is null.
    if (bin.op == "??")
        return lowerCoalesce(bin);
    // §25: a user-defined operator overload — sema resolved the binary
    // expression to a static `operator` method; call it with the two
    // operands.
    if (auto* opm =
            dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(bin))) {
        if (opm->isOperator) {
            llvm::Value* ol = lowerExpr(*bin.left);
            llvm::Value* orr = lowerExpr(*bin.right);
            if (!ol || !orr) return nullptr;
            std::vector<llvm::Value*> args{ol, orr};
            return emitMethodCall(opm, args, /*baseQualified=*/false);
        }
    }
    // §17.5: `x in lo..hi` — true if lo <= x <= hi (inclusive) or
    // lo <= x < hi (half-open). The right operand is a RangeExpr, not a
    // value, so we intercept before the general lowerExpr path.
    if (bin.op == "in") {
        if (!bin.left || !bin.right) return nullptr;
        auto* range = dynamic_cast<const ast::RangeExpr*>(bin.right.get());
        if (range && range->lo && range->hi && !range->step) {
            llvm::Value* x = lowerExpr(*bin.left);
            llvm::Value* lo = lowerExpr(*range->lo);
            llvm::Value* hi = lowerExpr(*range->hi);
            if (!x || !lo || !hi) return nullptr;
            // x >= lo AND (x <= hi for inclusive, x < hi for half-open).
            llvm::Value* geL = builder_.CreateICmpSGE(x, lo);
            llvm::Value* leH = range->inclusive
                                   ? builder_.CreateICmpSLE(x, hi)
                                   : builder_.CreateICmpSLT(x, hi);
            return builder_.CreateAnd(geL, leH);
        }
        // §17.5: `c in s` — char membership test in a string.
        // True when the char c appears at any position in s.
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            if (rTy && rTy->kind == sema::TypeRef::Kind::Primitive &&
                rTy->primitive == sema::PrimitiveType::String) {
                auto* lTy = ctx_.exprType(*bin.left);
                if (lTy && lTy->kind == sema::TypeRef::Kind::Primitive &&
                    lTy->primitive == sema::PrimitiveType::Char) {
                    // Implement as: scan through string code points for a match.
                    llvm::Value* needle = lowerExpr(*bin.left);
                    llvm::Value* haystack = lowerExpr(*bin.right);
                    if (!needle || !haystack) return nullptr;
                    llvm::Value* len = loadStringLen(haystack);
                    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                    auto* idxS = entryAlloca(i64());
                    builder_.CreateStore(builder_.getInt64(0), idxS);
                    auto* cBB = llvm::BasicBlock::Create(llvm_, "cin.c", currentFn_);
                    auto* bBB = llvm::BasicBlock::Create(llvm_, "cin.b", currentFn_);
                    auto* sBB = llvm::BasicBlock::Create(llvm_, "cin.s", currentFn_);  // success
                    auto* eBB = llvm::BasicBlock::Create(llvm_, "cin.e", currentFn_);  // end
                    builder_.CreateBr(cBB);
                    builder_.SetInsertPoint(cBB);
                    llvm::Value* idx = builder_.CreateLoad(i64(), idxS);
                    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bBB, eBB);
                    builder_.SetInsertPoint(bBB);
                    llvm::Value* idxB = builder_.CreateLoad(i64(), idxS);
                    llvm::Value* cp = loadCodePoint(haystack, idxB);
                    llvm::Value* found = builder_.CreateICmpEQ(cp, needle, "cin.found");
                    auto* incBB = llvm::BasicBlock::Create(llvm_, "cin.inc", currentFn_);
                    builder_.CreateCondBr(found, sBB, incBB);
                    builder_.SetInsertPoint(incBB);
                    builder_.CreateStore(builder_.CreateAdd(idxB, builder_.getInt64(1)), idxS);
                    builder_.CreateBr(cBB);
                    builder_.SetInsertPoint(sBB);
                    auto* sBBEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(eBB);
                    builder_.SetInsertPoint(eBB);
                    auto* phi = builder_.CreatePHI(i1(), 2, "cin.result");
                    phi->addIncoming(builder_.getFalse(), cBB);  // loop exhausted
                    phi->addIncoming(builder_.getTrue(), sBBEnd); // found match
                    return phi;
                }
            }
        }
        return nullptr;  // other `in` forms (collection) not yet implemented
    }
    llvm::Value* l = lowerExpr(*bin.left);
    llvm::Value* r = lowerExpr(*bin.right);
    if (!l || !r) return nullptr;
    const auto& op = bin.op;
    // CG24 (§6.3): floating-point arithmetic and comparisons (IEEE 754,
    // no overflow/div0 checks). If one operand is FP and the other an
    // integer, promote the integer (§6.2 mixed-type promotion).
    if (l->getType()->isFloatingPointTy() ||
        r->getType()->isFloatingPointTy()) {
        bool lFP = l->getType()->isFloatingPointTy();
        bool rFP = r->getType()->isFloatingPointTy();
        // The result FP type is the wider of two FP operands (so `float`
        // mixed with `double` widens the float), else the lone FP type.
        llvm::Type* fty =
            (lFP && rFP)
                ? (l->getType()->getPrimitiveSizeInBits() >=
                           r->getType()->getPrimitiveSizeInBits()
                       ? l->getType()
                       : r->getType())
                : (lFP ? l->getType() : r->getType());
        auto toFP = [&](llvm::Value* v) -> llvm::Value* {
            if (!v->getType()->isFloatingPointTy())
                return builder_.CreateSIToFP(v, fty);  // §6.2 int promote
            if (v->getType() != fty)
                return builder_.CreateFPExt(v, fty);   // float → double
            return v;
        };
        l = toFP(l);
        r = toFP(r);
        if (op == "+") return builder_.CreateFAdd(l, r);
        if (op == "-") return builder_.CreateFSub(l, r);
        if (op == "*") return builder_.CreateFMul(l, r);
        if (op == "/") return builder_.CreateFDiv(l, r);
        if (op == "=") return builder_.CreateFCmpOEQ(l, r);
        // §6.3 (IEEE 754): `<>` is the negation of `=`, so it is true
        // when either operand is NaN — an *unordered* not-equal (UNE).
        // (`=` stays ordered-equal: `NaN = NaN` is false.)
        if (op == "<>") return builder_.CreateFCmpUNE(l, r);
        if (op == "<") return builder_.CreateFCmpOLT(l, r);
        if (op == ">") return builder_.CreateFCmpOGT(l, r);
        if (op == "<=") return builder_.CreateFCmpOLE(l, r);
        if (op == ">=") return builder_.CreateFCmpOGE(l, r);
        return nullptr;
    }
    // CG25b: unsigned integer operands select unsigned operations
    // (udiv/urem/lshr/unsigned compares, zext widening, uXXX overflow).
    bool uns = isUnsignedBinOperand(bin);
    // CG25 (§6.2): mixed-width integer operands — widen the narrower to
    // the wider (zero-extend for unsigned, sign-extend otherwise) so the
    // op and any overflow check run at one width.
    if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy() &&
        !l->getType()->isIntegerTy(1) && !r->getType()->isIntegerTy(1) &&
        l->getType() != r->getType()) {
        unsigned lw = l->getType()->getIntegerBitWidth();
        unsigned rw = r->getType()->getIntegerBitWidth();
        llvm::Type* wide = lw >= rw ? l->getType() : r->getType();
        if (lw < rw) l = uns ? builder_.CreateZExt(l, wide)
                             : builder_.CreateSExt(l, wide);
        if (rw < lw) r = uns ? builder_.CreateZExt(r, wide)
                             : builder_.CreateSExt(r, wide);
    }
    // CG27a (§4.6): string binary operations — intercept before the
    // integer arithmetic path (string variables are ptr-typed, not int).
    // When either operand is a null literal, fall through to pointer
    // comparison (`s = null` / `s <> null` is a reference/null check).
    {
        auto isStr = [&](const ast::Expr* e) {
            if (!e) return false;
            auto* rt = ctx_.exprType(*e);
            return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                   rt->primitive == sema::PrimitiveType::String;
        };
        // Check if an expression has an unresolved generic type (T) that
        // is concretely substituted to string via genericSubstSemaTy_.
        auto isGenericString = [&](const ast::Expr* e) {
            if (!e || genericSubstSemaTy_.empty()) return false;
            auto* rt = ctx_.exprType(*e);
            if (!rt || rt->kind != sema::TypeRef::Kind::Named ||
                rt->declaration) return false;
            // Unresolved generic param — check if any substitution is String.
            if (rt->astNode && rt->astNode->name &&
                rt->astNode->name->parts.size() == 1) {
                auto sit = genericSubstSemaTy_.find(
                    rt->astNode->name->parts[0]);
                if (sit != genericSubstSemaTy_.end() && sit->second) {
                    auto* srt = sit->second;
                    return srt->kind == sema::TypeRef::Kind::Primitive &&
                           srt->primitive == sema::PrimitiveType::String;
                }
            }
            return false;
        };
        bool eitherNull =
            (bin.left && dynamic_cast<const ast::NullLiteralExpr*>(
                              bin.left.get())) ||
            (bin.right && dynamic_cast<const ast::NullLiteralExpr*>(
                               bin.right.get()));
        // CG-nullable: `x = null` / `x <> null` for nullable value types
        // (primitives, and a nullable enum — issue #364). The tagged struct is
        // not ptr-typed, so the normal ptr-null check below wouldn't fire.
        // Extract the hasValue field.
        if (eitherNull && (op == "=" || op == "<>")) {
            // Determine which side is the nullable value (non-null side).
            const ast::Expr* nvExpr = nullptr;
            llvm::Value* nvVal = nullptr;
            if (bin.left && !dynamic_cast<const ast::NullLiteralExpr*>(bin.left.get())) {
                if (auto* rt = ctx_.exprType(*bin.left))
                    if (isNullableValueTy(rt)) { nvExpr = bin.left.get(); nvVal = l; }
            } else if (bin.right && !dynamic_cast<const ast::NullLiteralExpr*>(bin.right.get())) {
                if (auto* rt = ctx_.exprType(*bin.right))
                    if (isNullableValueTy(rt)) { nvExpr = bin.right.get(); nvVal = r; }
            }
            if (nvVal && nvVal->getType()->isStructTy()) {
                // hasValue field (index 0). isNull when hasValue=false.
                llvm::Value* hasVal = nullablePrimHasValue(nvVal);
                // = null → !hasValue; <> null → hasValue
                return op == "=" ? builder_.CreateNot(hasVal) : hasVal;
            }
        }
        // CG-nullable / issue #364: `x = v` / `x <> v` where ONE side is a
        // nullable value (the tagged {i8 hasValue; T value} struct) and the
        // other is a non-null plain value of the inner type T — e.g.
        // `(c: Color?) = Color.Green`, the core test that a null `enum?` is
        // distinct from any enum value (including the value-0 member). The
        // struct-equality path below (op==`=` && l isStructTy) would feed a
        // scalar T into emitStructEquality and crash; intercept here. The
        // null-safe `=` of §8.4: null = nonNull is false; otherwise the inner
        // T's `=`. So result = hasValue AND (innerValue = v).
        if ((op == "=" || op == "<>") && !eitherNull) {
            llvm::Value* structSide = nullptr;
            llvm::Value* plainSide = nullptr;
            const sema::TypeRef* nvTy = nullptr;
            if (bin.left) {
                if (auto* rt = ctx_.exprType(*bin.left))
                    if (isNullableValueTy(rt) && l->getType()->isStructTy()) {
                        structSide = l; plainSide = r; nvTy = rt;
                    }
            }
            if (!structSide && bin.right) {
                if (auto* rt = ctx_.exprType(*bin.right))
                    if (isNullableValueTy(rt) && r->getType()->isStructTy()) {
                        structSide = r; plainSide = l; nvTy = rt;
                    }
            }
            if (structSide && plainSide && nvTy &&
                plainSide->getType() == nullableValueStructTy(nvTy)
                                            ->getElementType(1)) {
                llvm::Value* hasVal = nullablePrimHasValue(structSide);
                // Inner equality of the value slot against the plain value,
                // computed by the inner (non-nullable) type's rule so an enum
                // compares its i32 value and a float would use FCmp.
                sema::TypeRef innerTy = *nvTy;
                innerTy.nullable = false;
                llvm::Value* innerEq = emitValueEquality(
                    nullablePrimValue(structSide), plainSide, &innerTy);
                llvm::Value* eq = builder_.CreateAnd(hasVal, innerEq);
                return op == "=" ? eq : builder_.CreateNot(eq);
            }
        }
        bool lIsStr = isStr(bin.left.get()) || isGenericString(bin.left.get());
        bool rIsStr = isStr(bin.right.get()) || isGenericString(bin.right.get());
        bool lIsPtr = l->getType()->isPointerTy();
        bool rIsPtr = r->getType()->isPointerTy();
        // Equality/inequality: use string ops when at least one side is a
        // known string AND both are ptr-typed (the other ptr is also a string
        // from a generic context where the sema type is unresolved T=string).
        bool useStrEq = (lIsStr || rIsStr) && lIsPtr && rIsPtr && !eitherNull;
        if (useStrEq && (op == "=" || op == "<>")) {
            if (op == "=") return emitStringEq(l, r);
            return builder_.CreateNot(emitStringEq(l, r));
        }
        // FINDING [2] / §4.6 / §28.9: release a concat OPERAND that is itself a
        // fresh +1 string intermediate this codegen produced — i.e. a nested
        // string `+` or interpolation (`isPlusOneStringProducer`), so its
        // lowered value has no named-local owner and no scope-end release armed
        // for it (the named-local release path only covers LocalDecl nodes). For
        // a chained concat `a+b+c` the inner `a+b` result is consumed by the
        // outer concat and would otherwise leak. The release runs only AFTER
        // emitStringConcat has produced the result (its fresh +1 copy), so the
        // intermediate is fully consumed first — no UAF, no double-free. A plain
        // identifier / member / call operand is NOT a +1 producer here, so a
        // borrowed value (named local, field, call result) is never released.
        auto releaseConcatIntermediate = [&](const ast::Expr* operandExpr,
                                             llvm::Value* operandVal) {
            if (operandExpr && isPlusOneStringProducer(operandExpr))
                emitStringRelease(operandVal);
        };
        // §4.6: string concat when at least one side is a known string and
        // both operands are ptr-typed (handles e.g. string.From(ch) + s where
        // the From(ch) result may not be sema-annotated as string yet).
        if ((lIsStr || rIsStr) && lIsPtr && rIsPtr && !eitherNull && op == "+") {
            llvm::Value* res = emitStringConcat(l, r);
            releaseConcatIntermediate(bin.left.get(), l);
            releaseConcatIntermediate(bin.right.get(), r);
            return res;
        }
        if (lIsStr && !eitherNull) {
            if (op == "=") return emitStringEq(l, r);
            if (op == "<>") return builder_.CreateNot(emitStringEq(l, r));
            if (op == "+") {
                // §4.6: string + T — if the right operand is not a string,
                // convert it via emitPrimitiveToString first.
                llvm::Value* rStr = r;
                auto* rt = ctx_.exprType(*bin.right);
                if (rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                    rt->primitive != sema::PrimitiveType::String) {
                    rStr = emitPrimitiveToString(r, rt->primitive);
                } else if (!r->getType()->isPointerTy()) {
                    // Fallback: r has unknown/non-primitive sema type but is
                    // an integer-typed LLVM value (e.g. char from a generic
                    // context T=char). Infer the type from the LLVM type and
                    // convert to string. i32 → Char (preserves code-point).
                    using P = sema::PrimitiveType;
                    if (r->getType()->isIntegerTy(1))
                        rStr = emitPrimitiveToString(r, P::Bool);
                    else if (r->getType()->isIntegerTy(32))
                        rStr = emitPrimitiveToString(r, P::Char);
                    else if (r->getType()->isIntegerTy(64))
                        rStr = emitPrimitiveToString(r, P::Long);
                    else if (r->getType()->isDoubleTy())
                        rStr = emitPrimitiveToString(r, P::Double);
                    else if (r->getType()->isFloatTy())
                        rStr = emitPrimitiveToString(r, P::Float);
                }
                llvm::Value* res = emitStringConcat(l, rStr);
                // FINDING [2]: release the left operand if it is a fresh +1
                // string intermediate (a nested `+`/interpolation). Also release
                // a freshly-converted right operand (emitPrimitiveToString
                // allocates a +1 temporary distinct from the original `r`); when
                // no conversion happened `rStr == r` is a borrow and is left
                // alone. Both run after the concat result is produced.
                releaseConcatIntermediate(bin.left.get(), l);
                if (rStr != r) emitStringRelease(rStr);
                return res;
            }
            // CG27b (§4.6): ordinal comparison operators.
            if (op == "<" || op == ">" || op == "<=" || op == ">=") {
                llvm::Value* cmp = emitStringOrdCmp(l, r);
                llvm::Value* zero = llvm::ConstantInt::get(i32(), 0);
                if (op == "<") return builder_.CreateICmpSLT(cmp, zero);
                if (op == ">") return builder_.CreateICmpSGT(cmp, zero);
                if (op == "<=") return builder_.CreateICmpSLE(cmp, zero);
                /* >= */ return builder_.CreateICmpSGE(cmp, zero);
            }
        }
        // §4.6: primitive + string — convert lhs to string first.
        if (isStr(bin.right.get()) && op == "+" && !eitherNull) {
            auto* lt = ctx_.exprType(*bin.left);
            if (lt && lt->kind == sema::TypeRef::Kind::Primitive &&
                lt->primitive != sema::PrimitiveType::String) {
                llvm::Value* lStr = emitPrimitiveToString(l, lt->primitive);
                llvm::Value* res = emitStringConcat(lStr, r);
                // FINDING [2]: lStr is a fresh +1 conversion temporary; release
                // it (consumed by the concat). Release the right operand too if
                // it is a fresh +1 string intermediate. Both after the result.
                emitStringRelease(lStr);
                releaseConcatIntermediate(bin.right.get(), r);
                return res;
            }
        }
    }
    // CG22 (§6.1): integer + - * are overflow-checked; / and mod also
    // raise on a zero divisor and on INT_MIN / -1. Applies to integer
    // operands only (bool uses and/or; floats — CG24 — are unchecked).
    bool checkedInt = l->getType()->isIntegerTy() &&
                      !l->getType()->isIntegerTy(1);
    if (checkedInt) {
        if (op == "+")
            return emitCheckedArith(
                uns ? llvm::Intrinsic::uadd_with_overflow
                    : llvm::Intrinsic::sadd_with_overflow, l, r);
        if (op == "-")
            return emitCheckedArith(
                uns ? llvm::Intrinsic::usub_with_overflow
                    : llvm::Intrinsic::ssub_with_overflow, l, r);
        if (op == "*")
            return emitCheckedArith(
                uns ? llvm::Intrinsic::umul_with_overflow
                    : llvm::Intrinsic::smul_with_overflow, l, r);
        if (op == "/" || op == "mod") {
            emitGuardThrow(builder_.CreateICmpEQ(r, zeroLike(r)),
                           "DivideByZeroException");
            if (uns)
                return op == "/" ? builder_.CreateUDiv(l, r)
                                 : builder_.CreateURem(l, r);
            // Signed: INT_MIN / -1 (and mod) overflow the result type.
            llvm::Value* minV = minIntLike(r);
            llvm::Value* negOne =
                llvm::ConstantInt::getSigned(r->getType(), -1);
            emitGuardThrow(
                builder_.CreateAnd(builder_.CreateICmpEQ(l, minV),
                                   builder_.CreateICmpEQ(r, negOne)),
                "OverflowException");
            return op == "/" ? builder_.CreateSDiv(l, r)
                             : builder_.CreateSRem(l, r);
        }
    }
    if (op == "+") return builder_.CreateAdd(l, r);
    if (op == "-") return builder_.CreateSub(l, r);
    if (op == "*") return builder_.CreateMul(l, r);
    if (op == "/") return uns ? builder_.CreateUDiv(l, r)
                              : builder_.CreateSDiv(l, r);
    if (op == "mod") return uns ? builder_.CreateURem(l, r)
                                : builder_.CreateSRem(l, r);
    // Bitwise ops only valid on integer-typed operands — pointers (string refs)
    // would crash LLVM IR construction.
    if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
        if (op == "&") return builder_.CreateAnd(l, r);
        if (op == "|") return builder_.CreateOr(l, r);
        if (op == "^") return builder_.CreateXor(l, r);
        if (op == "<<") return builder_.CreateShl(l, r);
        if (op == ">>") return uns ? builder_.CreateLShr(l, r)
                                   : builder_.CreateAShr(l, r);
    }
    // §8.3 (equality table) / §15.1.2: inline arrays `[N]T` compare
    // element-by-element value equality — dispatch before the scalar integer
    // path (ICmp doesn't accept aggregate/array operands). An inline array
    // lowers to an LLVM ArrayType, not a StructType, so the record branch below
    // does not catch it. Recover the array's element type from either operand so
    // per-element comparison knows the Bezel element type (string elements
    // compare by content, record elements recurse — per §15.1.2).
    if ((op == "=" || op == "<>") && l->getType()->isArrayTy()) {
        const sema::TypeRef* elemTy = nullptr;
        for (const ast::Expr* side : {bin.left.get(), bin.right.get()}) {
            if (!side) continue;
            if (auto* t = ctx_.exprType(*side))
                if (t->kind == sema::TypeRef::Kind::InlineArray &&
                    t->arrayElement) {
                    elemTy = t->arrayElement; break;
                }
        }
        llvm::Value* eq = emitInlineArrayEquality(
            l, r, llvm::cast<llvm::ArrayType>(l->getType()), elemTy);
        return op == "=" ? eq : builder_.CreateNot(eq);
    }
    // §8.4 / issue #416: BOTH sides are nullable value types (the tagged
    // {i8 hasValue; T value} struct) — e.g. `record? = record?`, `int? = int?`,
    // `enum? = enum?`. Route to emitValueEquality's nullable-value branch, which
    // applies the null-safe `=` (both-null → equal; one-null → not-equal;
    // neither-null → inner T equality). This must precede the bare struct path
    // below: a nullable RECORD now lowers to the tagged struct too, so without
    // this it would reach emitStructEquality with the wrong (tag) shape.
    if ((op == "=" || op == "<>") && l->getType()->isStructTy() &&
        l->getType() == r->getType()) {
        const sema::TypeRef* nvTy = nullptr;
        for (const ast::Expr* side : {bin.left.get(), bin.right.get()}) {
            if (!side) continue;
            if (auto* t = ctx_.exprType(*side))
                if (isNullableValueTy(t) &&
                    l->getType() == nullableValueStructTy(t)) {
                    nvTy = t; break;
                }
        }
        if (nvTy) {
            llvm::Value* eq = emitValueEquality(l, r, nvTy);
            return op == "=" ? eq : builder_.CreateNot(eq);
        }
    }
    // CG29b (§8.3): records compare structurally — dispatch before the
    // scalar integer path (ICmp doesn't accept struct operands). Recover the
    // record's declared type from either operand so per-field comparison knows
    // the Bezel field types (string fields compare by content — per §8.3).
    if ((op == "=" || op == "<>") && l->getType()->isStructTy()) {
        const sema::TypeSymbol* recTy = nullptr;
        for (const ast::Expr* side : {bin.left.get(), bin.right.get()}) {
            if (!side) continue;
            if (auto* t = ctx_.exprType(*side))
                if (t->kind == sema::TypeRef::Kind::Named && t->declaration &&
                    t->declaration->kind == ast::TypeDeclKind::Record) {
                    recTy = t->declaration; break;
                }
        }
        llvm::Value* eq = emitStructEquality(
            l, r, llvm::cast<llvm::StructType>(l->getType()), recTy);
        return op == "=" ? eq : builder_.CreateNot(eq);
    }
    // Guard against type mismatch when one operand is a null literal
    // (ptrTy()) and the other is a non-pointer value type (e.g. `n is null`
    // desugars to `n = null` but `n: int` can never be null).
    {
    bool eitherNull2 =
        (bin.left && dynamic_cast<const ast::NullLiteralExpr*>(bin.left.get())) ||
        (bin.right && dynamic_cast<const ast::NullLiteralExpr*>(bin.right.get()));
    if (l->getType() != r->getType()) {
        if ((op == "=" || op == "<>") && eitherNull2) {
            // Type mismatch with a null literal: one side is a null ptr,
            // the other is a non-pointer (value) type that can never be null.
            bool lIsPtr = l->getType()->isPointerTy();
            bool rIsPtr = r->getType()->isPointerTy();
            if (lIsPtr && rIsPtr) {
                // Both pointer — compare normally (fall through).
            } else if (lIsPtr) {
                // Left is ptr, right is non-ptr value — compare left to null.
                llvm::Value* eq = builder_.CreateICmpEQ(
                    l, llvm::ConstantPointerNull::get(ptrTy()));
                return op == "=" ? eq : builder_.CreateNot(eq);
            } else if (rIsPtr) {
                // Right is ptr (the null literal), left is non-ptr — non-ptr
                // can never be null: = null is always false, <> null is true.
                return builder_.getInt1(op == "<>");
            } else {
                // Neither is a ptr — both are value types (shouldn't be null).
                return builder_.getInt1(op == "<>");
            }
        }
        // For other mismatched integer comparisons, extend to match widths.
        if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
            unsigned lb = l->getType()->getIntegerBitWidth();
            unsigned rb = r->getType()->getIntegerBitWidth();
            if (lb < rb) l = builder_.CreateSExt(l, r->getType());
            else r = builder_.CreateSExt(r, l->getType());
        }
    }
    } // end eitherNull2 guard
    // CG4: integer comparisons → i1.
    if (op == "=") return builder_.CreateICmpEQ(l, r);
    if (op == "<>") return builder_.CreateICmpNE(l, r);
    if (op == "<") return uns ? builder_.CreateICmpULT(l, r)
                              : builder_.CreateICmpSLT(l, r);
    if (op == ">") return uns ? builder_.CreateICmpUGT(l, r)
                              : builder_.CreateICmpSGT(l, r);
    if (op == "<=") return uns ? builder_.CreateICmpULE(l, r)
                               : builder_.CreateICmpSLE(l, r);
    if (op == ">=") return uns ? builder_.CreateICmpUGE(l, r)
                               : builder_.CreateICmpSGE(l, r);
    // CG4: logical and/or on i1. NOTE: §17 specifies short-circuit
    // evaluation; this eager form is observationally identical while
    // operands are side-effect-free (no calls lowered yet) — true
    // short-circuiting lands once call lowering exists.
    return nullptr;  // `and`/`or` handled by lowerShortCircuit
}


llvm::Value* Emitter::lowerShortCircuit(const ast::BinaryExpr& bin) {
    bool isAnd = (bin.op == "and");
    llvm::Value* l = lowerExpr(*bin.left);
    if (!l) return nullptr;
    auto* entryBB = builder_.GetInsertBlock();
    auto* rhsBB = llvm::BasicBlock::Create(llvm_, "sc.rhs", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "sc.end", currentFn_);
    // and: evaluate RHS only when l is true; or: only when l is false.
    if (isAnd) builder_.CreateCondBr(l, rhsBB, endBB);
    else       builder_.CreateCondBr(l, endBB, rhsBB);

    builder_.SetInsertPoint(rhsBB);
    llvm::Value* r = lowerExpr(*bin.right);
    if (!r) return nullptr;
    auto* rhsEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(i1(), 2);
    // Short-circuit value when RHS is skipped: false for `and`, true
    // for `or`.
    phi->addIncoming(builder_.getInt1(!isAnd), entryBB);
    phi->addIncoming(r, rhsEndBB);
    return phi;
}


llvm::Value* Emitter::lowerCoalesce(const ast::BinaryExpr& bin) {
    llvm::Value* l = lowerExpr(*bin.left);
    if (!l) return nullptr;
    // CG-nullable: `int? ?? expr` (and `enum? ?? expr` — issue #364) — the LHS
    // is the tagged struct {i8 hasValue; T value}. If hasValue is true, return
    // the inner value; otherwise evaluate and return the rhs. For an enum the
    // inner value (and the rhs, e.g. `Color.Red`) is the underlying i32.
    if (!l->getType()->isPointerTy() && bin.left) {
        if (auto* lt = ctx_.exprType(*bin.left)) {
            if (isNullableValueTy(lt)) {
                auto* entryBB = builder_.GetInsertBlock();
                auto* rhsBB = llvm::BasicBlock::Create(llvm_, "coal.rhs", currentFn_);
                auto* endBB = llvm::BasicBlock::Create(llvm_, "coal.end", currentFn_);
                llvm::Value* hasVal = nullablePrimHasValue(l);
                llvm::Value* primVal = nullablePrimValue(l);
                builder_.CreateCondBr(hasVal, endBB, rhsBB);
                builder_.SetInsertPoint(rhsBB);
                llvm::Value* r = lowerExpr(*bin.right);
                bool rhsDiv = diverged_;
                if (!r) return nullptr;
                llvm::BasicBlock* rhsEndBB = nullptr;
                if (!rhsDiv) { rhsEndBB = builder_.GetInsertBlock(); builder_.CreateBr(endBB); }
                builder_.SetInsertPoint(endBB);
                diverged_ = false;
                if (rhsDiv) return primVal;
                auto* phi = builder_.CreatePHI(primVal->getType(), 2);
                phi->addIncoming(primVal, entryBB);
                phi->addIncoming(r, rhsEndBB);
                return phi;
            }
        }
        return nullptr;  // other non-ptr — cannot coalesce
    }
    auto* entryBB = builder_.GetInsertBlock();
    auto* rhsBB = llvm::BasicBlock::Create(llvm_, "coalesce.rhs", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "coalesce.end", currentFn_);
    llvm::Value* isNull = builder_.CreateICmpEQ(
        l, llvm::ConstantPointerNull::get(ptrTy()));
    builder_.CreateCondBr(isNull, rhsBB, endBB);

    builder_.SetInsertPoint(rhsBB);
    llvm::Value* r = lowerExpr(*bin.right);
    bool rhsDiverged = diverged_;  // CG40: e.g. `a ?? throw E`
    if (!r) return nullptr;
    llvm::BasicBlock* rhsEndBB = nullptr;
    if (!rhsDiverged) {
        rhsEndBB = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
    }

    builder_.SetInsertPoint(endBB);
    diverged_ = false;  // the result is reachable via the non-null path
    if (rhsDiverged) {
        // The right operand branched away (threw); endBB is reached only
        // by the non-null left path, so the result is always `l`.
        return l;
    }
    auto* phi = builder_.CreatePHI(ptrTy(), 2);
    phi->addIncoming(l, entryBB);  // left was non-null
    phi->addIncoming(r, rhsEndBB);
    return phi;
}


llvm::Value* Emitter::lowerUnary(const ast::UnaryExpr& un) {
    if (!un.operand) return nullptr;
    // §25: a user-defined unary operator overload — sema resolved this
    // unary expression to a static `operator` method; call it with the
    // operand.
    if (auto* opm =
            dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(un))) {
        if (opm->isOperator) {
            llvm::Value* ov = lowerExpr(*un.operand);
            if (!ov) return nullptr;
            std::vector<llvm::Value*> args{ov};
            return emitMethodCall(opm, args, /*baseQualified=*/false);
        }
    }
    // issue #409 / §28.8.1 / §28.11: `move coll[i]` is the slot-take form on an
    // owning collection. Unlike `move local` / `move x.f` (a pure compile-time
    // ownership transfer whose runtime value is the operand unchanged), the
    // collection slot must be left moved-from so the collection's later Dispose
    // does not re-free the element the new owner now owns. Handle it here, before
    // the generic operand lowering, so the slot read + null happen exactly once
    // (and the index/key expression is evaluated only once). For a borrowing
    // collection (where sema rejects `move`) the helper returns null and we fall
    // through to the ordinary read, which does NOT null the slot.
    if (un.op == "move")
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(un.operand.get()))
            if (llvm::Value* moved = lowerMoveFromCollectionSlot(*ix))
                return moved;

    // §30.2: `await <task>` — the async suspend point. Handled here, before the
    // generic operand lowering, because the await owns its operand evaluation
    // (lower the task, fast-path or suspend, then read the result). Only valid
    // inside an async state machine (smKind_ == Await); sema rejects `await`
    // elsewhere, so the guard inside lowerAwait is defensive.
    if (un.op == "await")
        return lowerAwait(un);

    llvm::Value* v = lowerExpr(*un.operand);
    if (!v) return nullptr;
    const auto& op = un.op;
    if (op == "-") {
        if (v->getType()->isFloatingPointTy())  // CG24: §6.3 IEEE negate
            return builder_.CreateFNeg(v);
        // §6.1: unary negation overflows only at INT_MIN.
        if (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(1)) {
            emitGuardThrow(builder_.CreateICmpEQ(v, minIntLike(v)),
                           "OverflowException");
        }
        return builder_.CreateNeg(v);
    }
    if (op == "~") return builder_.CreateNot(v);   // bitwise complement
    if (op == "+") return v;                       // identity
    if (op == "not") return builder_.CreateNot(v); // i1 logical negation
    // §28.3: `move x` is a compile-time ownership transfer; at runtime
    // it yields the operand value unchanged (the same reference).
    if (op == "move") return v;
    return nullptr;
}


llvm::FunctionCallee Emitter::callocFn() {
    auto* fnTy = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
    return module_->getOrInsertFunction("calloc", fnTy);
}


llvm::FunctionCallee Emitter::freeFn() {
    // #380: libc free — the counterpart to callocFn() used to reclaim a
    // disposed class instance's heap block at the most-derived Dispose.
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, false);
    return module_->getOrInsertFunction("free", fnTy);
}


llvm::Value* Emitter::lowerLambda(const ast::LambdaExpr& lam) {
    const std::vector<sema::Symbol*>* paramSyms = ctx_.lambdaParams(&lam);

    // Build user-visible parameter type list.
    std::vector<llvm::Type*> userParamTys;
    if (paramSyms) {
        std::size_t pi = 0;
        for (sema::Symbol* ps : *paramSyms) {
            auto* par = dynamic_cast<sema::ParameterSymbol*>(ps);
            llvm::Type* ty = i32();
            if (par && par->declType) {
                if (sema::TypeRef* rt = ctx_.resolvedType(*par->declType))
                    ty = lowerTypeRef(rt);
            }
            // If the param type is still i32 (unresolved) but we have an
            // expected function type from the call site, infer from that.
            // Also propagate the sema TypeRef to the ParameterSymbol so that
            // identifier lookups in the lambda body (e.g. s.Length) get the
            // correct type (e.g. string → ptr, enabling string subscript codegen).
            if (ty == i32() && expectedFunctionType_ &&
                pi < expectedFunctionType_->funcParams.size() &&
                expectedFunctionType_->funcParams[pi].type) {
                sema::TypeRef* expRT = expectedFunctionType_->funcParams[pi].type;
                llvm::Type* expected = lowerTypeRef(expRT);
                if (expected != i32()) {
                    ty = expected;
                    // Backfill: give the ParameterSymbol a resolved sema type so
                    // that body identifier references pick up the right TypeRef.
                    if (par && !par->declType)
                        lambdaParamTypeHint_[par] = expRT;
                }
            }
            userParamTys.push_back(ty);
            ++pi;
        }
    }

    // Pre-determine return type from sema's exprType of the body.
    llvm::Type* retTy = i32();
    if (lam.exprBody) {
        if (sema::TypeRef* rt = ctx_.exprType(*lam.exprBody))
            retTy = lowerTypeRef(rt);
    }
    // If return type is still i32 (unresolved generic), try the expected
    // function type's return type (from the call site context).
    if (retTy == i32() && expectedFunctionType_ &&
        expectedFunctionType_->funcReturn) {
        llvm::Type* expected = lowerTypeRef(expectedFunctionType_->funcReturn);
        if (expected != i32()) retTy = expected;
    }
    // Also try to resolve via genericSubst_ when the sema return type is
    // an unresolved named generic param (e.g. C in Curried<A,B,C>).
    if (retTy == i32() && lam.exprBody) {
        if (sema::TypeRef* rt = ctx_.exprType(*lam.exprBody)) {
            if (rt->kind == sema::TypeRef::Kind::Named && !rt->declaration &&
                rt->astNode && rt->astNode->name &&
                rt->astNode->name->parts.size() == 1) {
                auto sit = genericSubst_.find(rt->astNode->name->parts[0]);
                if (sit != genericSubst_.end() && sit->second != i32())
                    retTy = sit->second;
            }
        }
    }

    // §27.5: captured variables are heap-allocated in a closure object and
    // shared by reference — a mutation inside the lambda is visible outside
    // and vice versa. We compute the capture set (the outer locals/params the
    // body actually references), promote each captured outer slot to a shared
    // heap cell, and store the cell *pointer* in the env. Both the enclosing
    // function and the lambda then load/store through the same cell.
    //
    // Each env field holds a `ptr`: a boxed capture stores the cell pointer;
    // the `this` capture stores the receiver pointer directly (by value, as
    // `this` is not reassignable so it needs no shared cell).
    struct CaptureEntry {
        llvm::Type*            valTy;     // the captured variable's value type
        llvm::Value*           envValue;  // value to store into the env field
        const sema::Symbol*    sym;   // non-null for param captures
        const ast::Node*       decl;  // non-null for local captures
        bool                   isThis = false;
    };
    std::vector<CaptureEntry> captures;

    // Save state (needed for capture slot access and restoration).
    auto savedLambdaParamHints = lambdaParamTypeHint_;  // restore after body
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    llvm::BasicBlock* savedBB = builder_.GetInsertBlock();
    auto savedRefParams = refParams_;
    auto savedLoopStack = loopStack_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::BasicBlock* savedProp = propagateBB_;
    llvm::Value* savedCaught = currentCaughtExc_;
    std::size_t savedUnwindDepth = unwindScopeDepth_;
    bool savedDiverged = diverged_;

    // §27.5: the set of outer locals/params the body references.
    std::unordered_set<const sema::Symbol*> capturedSyms;
    collectCapturedSyms(lam, capturedSyms);

    // Box and collect captured outer params. Boxing promotes the variable's
    // storage to a shared heap cell held in a fresh ptr-slot, which we rebind.
    // Collect the symbols first to avoid mutating the map while iterating.
    std::vector<const sema::Symbol*> capturedParamSyms;
    for (auto& [sym, slot] : paramSlots_) {
        (void)slot;
        if (capturedSyms.find(sym) != capturedSyms.end())
            capturedParamSyms.push_back(sym);
    }
    for (const sema::Symbol* sym : capturedParamSyms) {
        llvm::AllocaInst* slot = paramSlots_[sym];
        CaptureEntry ce;
        ce.valTy = cellVars_.count(slot) ? cellVars_[slot]
                                         : slot->getAllocatedType();
        llvm::AllocaInst* newSlot = nullptr;
        ce.envValue = boxLocalSlot(slot, newSlot);  // the shared cell pointer
        paramSlots_[sym] = newSlot;  // rebind to the ptr cell-holder
        rebindDisposalSlot(slot, newSlot);
        ce.sym = sym;
        ce.decl = nullptr;
        captures.push_back(ce);
    }
    // Box and collect captured outer locals.
    std::vector<const ast::Node*> capturedLocalDecls;
    for (auto& [decl, slot] : locals_) {
        (void)slot;
        for (const sema::Symbol* s : capturedSyms) {
            if (auto* loc = dynamic_cast<const sema::LocalSymbol*>(s)) {
                if (loc->decl == decl) { capturedLocalDecls.push_back(decl); break; }
            }
        }
    }
    for (const ast::Node* decl : capturedLocalDecls) {
        llvm::AllocaInst* slot = locals_[decl];
        CaptureEntry ce;
        ce.valTy = cellVars_.count(slot) ? cellVars_[slot]
                                         : slot->getAllocatedType();
        llvm::AllocaInst* newSlot = nullptr;
        ce.envValue = boxLocalSlot(slot, newSlot);  // the shared cell pointer
        locals_[decl] = newSlot;  // rebind to the ptr cell-holder
        rebindDisposalSlot(slot, newSlot);
        ce.sym = nullptr;
        ce.decl = decl;
        captures.push_back(ce);
    }

    // Capture `this` if we're inside an instance method — the lambda body
    // may access instance fields via implicit this. `this` is captured by
    // value (it is not reassignable, so no shared cell is needed).
    if (currentThis_ != nullptr) {
        CaptureEntry ce;
        ce.valTy = ptrTy();
        ce.envValue = currentThis_;
        ce.sym = nullptr;
        ce.decl = nullptr;
        ce.isThis = true;
        captures.push_back(ce);
    }

    // Build env struct type. Every field is a `ptr`: a boxed capture stores
    // its cell pointer; the `this` capture stores the receiver pointer.
    std::vector<llvm::Type*> envFieldTys;
    for (const auto& ce : captures) {
        (void)ce;
        envFieldTys.push_back(ptrTy());
    }

    // Build the LLVM function type: (ptr env, user_params...) -> ret.
    std::vector<llvm::Type*> allParamTys;
    allParamTys.push_back(ptrTy());  // env_ptr (slot 0)
    for (llvm::Type* t : userParamTys) allParamTys.push_back(t);

    static std::atomic<unsigned> lamCounter{0};
    unsigned lamId = lamCounter.fetch_add(1);
    std::string lamName = "lam$" + std::to_string(lamId);
    auto* fnTy = llvm::FunctionType::get(retTy, allParamTys, false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, lamName, module_.get());

    // --- Emit the lambda function body ---
    // Snapshot the slot/cell state *after* boxing the outer slots so the
    // outer-frame rebindings persist when restored (the enclosing function
    // keeps accessing the now-shared variables through their cells, via the
    // rebound ptr-slots, after the lambda returns). These supersede the
    // pre-boxing state.
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    auto savedCellVars = cellVars_;
    auto savedDisposeStack = disposeStack_;
    currentFn_ = fn;
    currentThis_ = nullptr;
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    cellVars_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    unwindTarget_ = nullptr;
    propagateBB_ = nullptr;
    currentCaughtExc_ = nullptr;
    unwindScopeDepth_ = 0;
    diverged_ = false;
    builder_.SetInsertPoint(
        llvm::BasicBlock::Create(llvm_, "entry", fn));

    // Arg 0 is env_ptr; bind captured vars from it.
    llvm::Value* envPtr = &*fn->args().begin();
    if (!captures.empty() && !envFieldTys.empty()) {
        auto* envTy = llvm::StructType::get(llvm_, envFieldTys);
        for (std::size_t i = 0; i < captures.size(); ++i) {
            const CaptureEntry& ce = captures[i];
            llvm::Value* fieldPtr = builder_.CreateStructGEP(
                envTy, envPtr, static_cast<unsigned>(i));
            // The env field holds a pointer: the shared cell (boxed captures)
            // or the receiver (`this`).
            llvm::Value* ptrVal = builder_.CreateLoad(ptrTy(), fieldPtr);
            if (ce.isThis) {
                // Restore 'this' from the captured receiver pointer.
                currentThis_ = ptrVal;
            } else {
                // §27.5: bind the captured variable to its shared cell. The
                // slot holds the cell pointer; cellVars_ marks it boxed so
                // reads/writes in the body route through the cell — sharing
                // storage with the enclosing function.
                auto* slot = entryAlloca(ptrTy());
                builder_.CreateStore(ptrVal, slot);
                cellVars_[slot] = ce.valTy;
                if (ce.sym) paramSlots_[ce.sym] = slot;
                if (ce.decl) locals_[ce.decl] = slot;
            }
        }
    }

    // Bind user lambda params (args 1..N).
    if (paramSyms) {
        std::size_t idx = 0;
        for (auto it = std::next(fn->args().begin()); it != fn->args().end();
             ++it, ++idx) {
            if (idx >= paramSyms->size()) break;
            auto* slot = entryAlloca(it->getType());
            builder_.CreateStore(&*it, slot);
            paramSlots_[(*paramSyms)[idx]] = slot;
        }
    }

    // Emit body.
    if (lam.exprBody) {
        llvm::Value* v = lowerExpr(*lam.exprBody);
        if (!builder_.GetInsertBlock()->getTerminator()) {
            if (v && !retTy->isVoidTy()) builder_.CreateRet(v);
            else builder_.CreateRetVoid();
        }
    } else {
        lowerScopedBlock(lam.stmts);
        // Fall-through terminator: must match the lambda's actual return type.
        // A block-bodied handler/lambda whose declared return type is non-void
        // (retTy != void) would otherwise get a `ret void` here — malformed IR
        // (ret-type mismatch). Mirror emitImplicitReturn: void → retvoid, else
        // a zero-initialized value of retTy.
        if (!builder_.GetInsertBlock()->getTerminator()) {
            if (retTy->isVoidTy()) builder_.CreateRetVoid();
            else builder_.CreateRet(llvm::Constant::getNullValue(retTy));
        }
    }

    // --- Restore emitter state ---
    lambdaParamTypeHint_ = std::move(savedLambdaParamHints);
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    refParams_ = std::move(savedRefParams);
    cellVars_ = std::move(savedCellVars);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    unwindTarget_ = savedUnwind;
    propagateBB_ = savedProp;
    currentCaughtExc_ = savedCaught;
    unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    if (savedBB) builder_.SetInsertPoint(savedBB);

    // --- Build the closure struct { fn_ptr, env_ptr, rc } ---
    // #422 / §27.5 / §28.9: allocate a heap closure cell { ptr fn, ptr env,
    // i64 rc } (24 bytes). The rc is appended after env so fn (off 0) and env
    // (off 8) keep their offsets; getTypeAllocSize is the single source of truth
    // for the size so the rc store never overflows.
    uint64_t cellSz =
        module_->getDataLayout().getTypeAllocSize(closureCellType());
    llvm::Value* closureObj = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), builder_.getInt64((int64_t)cellSz)});
    // Store fn_ptr at offset 0.
    builder_.CreateStore(fn, closureObj);
    // Allocate and fill the env struct (if there are captures).
    llvm::Value* envAlloc = llvm::ConstantPointerNull::get(ptrTy());
    if (!captures.empty()) {
        auto* envTy = llvm::StructType::get(llvm_, envFieldTys);
        llvm::Value* envSize = builder_.CreatePtrToInt(
            builder_.CreateGEP(envTy, llvm::ConstantPointerNull::get(ptrTy()),
                               {builder_.getInt32(1)}),
            i64());
        envAlloc = builder_.CreateCall(callocFn(),
                                       {builder_.getInt64(1), envSize});
        for (std::size_t i = 0; i < captures.size(); ++i) {
            const CaptureEntry& ce = captures[i];
            // §27.5: store the shared cell pointer (boxed captures) or the
            // receiver pointer (`this`) into the env. Both are pointers.
            llvm::Value* dst = builder_.CreateStructGEP(
                envTy, envAlloc, static_cast<unsigned>(i));
            builder_.CreateStore(ce.envValue, dst);
        }
    }
    // Store env_ptr at offset sizeof(ptr).
    llvm::Value* envSlot = builder_.CreateGEP(
        ptrTy(), closureObj, {builder_.getInt32(1)});
    builder_.CreateStore(envAlloc, envSlot);
    initClosureRc(closureObj);  // #422: producer's reference, rc := 1
    return closureObj;
}


llvm::Value* Emitter::lowerWith(const ast::WithExpr& we) {
    // `expr with name := v, ... end` has two spec forms, distinguished by
    // the source type:
    //  - Record (value type, §9.5): a *non-destructive copy* — the source
    //    record is unchanged. The source is a first-class struct value, so
    //    spill it to a fresh stack slot (the copy), overwrite the named
    //    fields through that pointer, and reload the result by value. Using
    //    the struct value directly as a GEP base is malformed (and hung the
    //    compiler — #254).
    //  - Class (object initializer block, §22.11): the source is a freshly
    //    `new`-constructed instance (a pointer); the named members are
    //    assigned in place and the same pointer is returned.
    if (!we.source) return nullptr;
    llvm::Value* src = lowerExpr(*we.source);
    if (!src) return nullptr;
    auto* rt = ctx_.exprType(*we.source);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return src;
    const sema::TypeSymbol* t = rt->declaration;
    const bool isRecord = t->kind == ast::TypeDeclKind::Record;
    // classStruct routes records to recordStruct (no vtable), so this is
    // the right layout for both forms.
    llvm::StructType* st = classStruct(t);
    // The GEP base: a spilled copy for a record, the instance pointer for
    // a class.
    llvm::Value* base = src;
    if (isRecord) {
        base = entryAlloca(st);
        builder_.CreateStore(src, base);
    }
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(t, storage);
    for (const auto& a : we.assigns) {
        if (!a.value) continue;
        const sema::Symbol* m = nullptr;
        for (const sema::Symbol* s : storage)
            if (s->name == a.name) { m = s; break; }
        if (!m) continue;
        auto idxIt = fieldIndex_.find(m);
        if (idxIt == fieldIndex_.end()) continue;
        llvm::Value* v = lowerExpr(*a.value);
        if (!v) continue;
        builder_.CreateStore(
            v, builder_.CreateStructGEP(st, base, idxIt->second));
    }
    // Record: return the modified copy by value. Class: return the mutated
    // instance pointer.
    return isRecord ? builder_.CreateLoad(st, base) : base;
}


llvm::Value* Emitter::lowerNew(const ast::NewExpr& ne) {
    if (!ne.type) return nullptr;
    sema::TypeRef* rt = ctx_.resolvedType(*ne.type);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return nullptr;
    // per §14.2: `new T()` inside a generic body. T is a type parameter (its
    // TypeRef has no declaration); resolve it to the concrete sema TypeRef of
    // the active monomorphization so construction dispatches to the substituted
    // type — a class's parameterless constructor for a class substitution, or
    // `default(T)` for a record substitution. Without this the bare type-param
    // TypeRef falls through every construction branch and the constructor is
    // never called (issue #361 / D12).
    if (!rt->declaration && rt->astNode && rt->astNode->name &&
        rt->astNode->name->parts.size() == 1 &&
        !genericSubstSemaTy_.empty()) {
        auto sit = genericSubstSemaTy_.find(rt->astNode->name->parts[0]);
        if (sit != genericSubstSemaTy_.end() && sit->second &&
            sit->second->kind == sema::TypeRef::Kind::Named &&
            sit->second->declaration) {
            rt = const_cast<sema::TypeRef*>(sit->second);
        }
    }
    if (!rt->declaration) {
        // CG22: built-in exception types (Exception, ArgumentException, …)
        // have no user TypeSymbol (rt->declaration == null) but do have a
        // built-in vtable from setupBuiltinExceptions. Construct a 16-byte
        // object — [vtable_ptr, message_ptr] — so that e.Message works.
        if (ne.type->name && !ne.type->name->parts.empty()) {
            std::string n(ne.type->name->parts.back());
            auto vit = builtinVtable_.find(n);
            if (vit != builtinVtable_.end()) {
                // 16 bytes: slot0=vtable, slot1=message string ptr
                llvm::Value* obj = builder_.CreateCall(
                    callocFn(), {builder_.getInt64(1), builder_.getInt64(16)});
                builder_.CreateStore(vit->second, obj);
                // Store message string if a first argument was provided.
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* msgStr = lowerExpr(*ne.args[0].value);
                    if (msgStr) {
                        auto* i8Ty2 = llvm::Type::getInt8Ty(llvm_);
                        llvm::Value* msgSlot = builder_.CreateGEP(
                            i8Ty2, obj, builder_.getInt64(8));
                        builder_.CreateStore(msgStr, msgSlot);
                    }
                }
                return obj;
            }
        }
        // §3: stdlib collection types — allocate via runtime.
        if (ne.type->name && !ne.type->name->parts.empty()) {
            std::string_view colName = ne.type->name->parts[0];
            // Determine element size from the first generic arg (if present).
            // For pointer-typed elements (class, string) → sizeof(ptr) = 8.
            // For int → 4, long → 8, double → 8, bool → 1, etc.
            int64_t elemSize = 8;  // default: pointer
            if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                llvm::Type* et = lowerTypeRef(rt->genericArgs[0]);
                if (et) {
                    if (et->isIntegerTy(32) || et->isFloatTy()) elemSize = 4;
                    else if (et->isIntegerTy(1) || et->isIntegerTy(8)) elemSize = 1;
                    else if (et->isIntegerTy(16)) elemSize = 2;
                    else elemSize = 8;  // ptr, i64, double, struct
                }
            }
            // issues #343 / D8 / §4.10: a Set<T> (or OwnedSet<T>) is backed by
            // the generic-key hash table so equality and hashing follow T's
            // contract (§8.3) — real set semantics (dedup by content). This
            // covers records (#343), primitives/enums, AND strings (D8: the
            // array backing cannot deduplicate). Stored value is a non-null
            // sentinel (membership only).
            if ((colName == "Set" || colName == "OwnedSet")) {
                uint64_t ksz = 0;
                if (const sema::TypeRef* elemTy =
                        genericBackingType(rt, 0, /*isSet=*/true, &ksz)) {
                    llvm::Function* hashT = buildValueHashTrampoline(elemTy);
                    llvm::Function* eqT   = buildValueEqTrampoline(elemTy);
                    auto* allocFn = declareC("bz_dict_alloc_generic", ptrTy(),
                                             {i64(), ptrTy(), ptrTy()});
                    return builder_.CreateCall(allocFn,
                        {builder_.getInt64((int64_t)ksz), hashT, eqT});
                }
            }
            // per §4.6/§4.7: LinkedList/OwnedLinkedList use the real doubly-linked-
            // list runtime (bz_llist_alloc), not the bz_array_t backing. The
            // elem_size argument tells the runtime how many bytes to store inline
            // in each node — same sizing as bz_array_t (4 for int/float, 8 otherwise).
            if (colName == "LinkedList" || colName == "OwnedLinkedList") {
                auto* allocFn = declareC("bz_llist_alloc", ptrTy(), {i64()});
                return builder_.CreateCall(allocFn, {builder_.getInt64(elemSize)});
            }
            if (colName == "Array" || colName == "OwnedArray" ||
                colName == "Stack" || colName == "OwnedStack" ||
                colName == "Queue" || colName == "OwnedQueue" ||
                colName == "Set" || colName == "OwnedSet") {
                auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                return builder_.CreateCall(allocFn,
                    {builder_.getInt64(elemSize), builder_.getInt64(16)});
            }
            if (colName == "Dictionary" || colName == "OwnedDictionary") {
                // issues #343 / D3 / §4.8: a non-string key uses the generic-key
                // runtime variant, parameterized by the key's hash/eq via
                // trampolines (§8.3). This covers records (#343) AND primitive/
                // enum keys (D3). The STRING key keeps the dedicated fast path
                // (bz_dict_alloc + the string-key entry points).
                uint64_t ksz = 0;
                if (const sema::TypeRef* keyTy =
                        genericBackingType(rt, 0, /*isSet=*/false, &ksz)) {
                    llvm::Function* hashT = buildValueHashTrampoline(keyTy);
                    llvm::Function* eqT   = buildValueEqTrampoline(keyTy);
                    auto* allocFn = declareC("bz_dict_alloc_generic", ptrTy(),
                                             {i64(), ptrTy(), ptrTy()});
                    return builder_.CreateCall(allocFn,
                        {builder_.getInt64((int64_t)ksz), hashT, eqT});
                }
                auto* allocFn = declareC("bz_dict_alloc", ptrTy(), {});
                return builder_.CreateCall(allocFn, {});
            }
            if (colName == "StringBuilder") {
                // StringBuilder is a bz_array_t of chars (elem_size=1 or 2).
                auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                return builder_.CreateCall(allocFn,
                    {builder_.getInt64(2), builder_.getInt64(64)});
            }
            if (colName == "Random") {
                int64_t seed = 0;
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* sv = lowerExpr(*ne.args[0].value);
                    if (sv && sv->getType()->isIntegerTy(64))
                        seed = 0;  // pass dynamic seed below
                    auto* allocFn = declareC("bz_random_alloc", ptrTy(), {i64()});
                    return builder_.CreateCall(allocFn, {sv ? sv : builder_.getInt64(0)});
                }
                auto* timeFn = declareC("bz_datetime_now_ticks", i64(), {});
                llvm::Value* ts = builder_.CreateCall(timeFn, {});
                auto* allocFn = declareC("bz_random_alloc", ptrTy(), {i64()});
                return builder_.CreateCall(allocFn, {ts});
            }
            // §6.1 / §30.8: new CancellationTokenSource() — allocate via
            // bz_cts_alloc (stdlib §6.1 default constructor).
            if (colName == "CancellationTokenSource") {
                auto* allocFn = declareC("bz_cts_alloc", ptrTy(), {});
                return builder_.CreateCall(allocFn, {}, "cts.alloc");
            }
            // §2.5: new DateTime(year, month, day) or
            //        new DateTime(year, month, day, hour, minute, second) or
            //        new DateTime(year, month, day, hour, minute, second, ms).
            // DateTime is a scalar i64 tick value — no heap allocation.
            if (colName == "DateTime") {
                auto getI32 = [&](size_t idx) -> llvm::Value* {
                    if (idx >= ne.args.size() || !ne.args[idx].value)
                        return builder_.getInt32(0);
                    llvm::Value* v = lowerExpr(*ne.args[idx].value);
                    if (!v) return builder_.getInt32(0);
                    if (!v->getType()->isIntegerTy(32))
                        v = builder_.CreateTrunc(v, i32());
                    return v;
                };
                llvm::Value* yr  = getI32(0);
                llvm::Value* mo  = getI32(1);
                llvm::Value* day = getI32(2);
                llvm::Value* hr  = getI32(3);
                llvm::Value* mn  = getI32(4);
                llvm::Value* sec = getI32(5);
                llvm::Value* ms  = getI32(6);
                auto* fn = declareC("bz_datetime_parts_to_ticks", i64(),
                    {i32(), i32(), i32(), i32(), i32(), i32(), i32()});
                return builder_.CreateCall(fn,
                    {yr, mo, day, hr, mn, sec, ms}, "dt.ctor");
            }
            // §2.5: new TimeSpan(ticks: long) — construct from raw tick count.
            // No heap allocation — TimeSpan is a scalar i64.
            if (colName == "TimeSpan") {
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* v = lowerExpr(*ne.args[0].value);
                    if (v && !v->getType()->isIntegerTy(64))
                        v = builder_.CreateSExt(v, i64());
                    return v ? v : builder_.getInt64(0);
                }
                return builder_.getInt64(0);  // default: zero TimeSpan
            }
        }
        return nullptr;
    }
    const sema::TypeSymbol* t = rt->declaration;

    // §9 / §9.5: `new Record(args)` builds a value-type struct directly —
    // no heap allocation, vtable, constructor chain, or disposal. This is
    // the spec's record construction form (records use `new`, like classes,
    // but produce a value).
    if (t->kind == ast::TypeDeclKind::Record)
        return buildRecordValue(t, ne.args);

    // CG-generic: if this is a generic instantiation (e.g. Box<int>),
    // set up the type-parameter substitution before classStruct so that
    // field types are computed with the concrete type arguments.
    auto savedSubst = genericSubst_;  // save outer subst (nested generics)
    auto savedSemaTy = genericSubstSemaTy_;
    if (!rt->genericArgs.empty() && t->decl) {
        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (td && td->genericParams.size() == rt->genericArgs.size()) {
            // Resolve generic args WITH the outer substitution active so
            // that `new Pair<T>` inside a `Pair<string>` method correctly
            // maps T → ptr (string) rather than the i32 fallback.
            std::unordered_map<std::string_view, llvm::Type*> newSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
            for (std::size_t gi = 0; gi < td->genericParams.size(); ++gi) {
                llvm::Type* concrTy = lowerTypeRef(rt->genericArgs[gi]);
                std::string_view pname = td->genericParams[gi].name;
                newSubst[pname] = concrTy;
                // Record the sema TypeRef for the concrete arg so the
                // binary `=` handler can distinguish string vs. class ptr.
                newSema[pname] = rt->genericArgs[gi];
            }
            genericSubst_ = std::move(newSubst);
            genericSubstSemaTy_ = std::move(newSema);
        }
    }

    llvm::StructType* st = classStruct(t);
    // Note: genericSubst_ stays active until after the constructor runs,
    // so that inline constructor bodies (for generic types) use concrete
    // types. It is restored at the end of lowerNew.

    // sizeof(st) via the null-GEP / ptrtoint idiom (no DataLayout dep).
    llvm::Value* sizeofSt = builder_.CreatePtrToInt(
        builder_.CreateGEP(st, llvm::ConstantPointerNull::get(ptrTy()),
                           {builder_.getInt32(1)}),
        i64());
    llvm::Value* obj = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), sizeofSt});

    // §14 / §2.5 / §2.6 (issue #328 FINDING [2]): an IMPORTED GENERIC type has
    // NO stable per-type symbols to link against — a generic is monomorphized
    // per-instantiation (§14), so the dependency emits no `bz$<Type>$init` (the
    // declareFunctions carve-out exports init only for non-generic public types)
    // and its `bz$<Type>$ctor` is the template (T→i32), not this instantiation.
    // Sema rejects `new ImportedGeneric<...>(...)` before codegen runs
    // (checkImportedNewExprArgs), so this branch is normally unreachable for a
    // generic imported type; the guard is a BACKSTOP so codegen never emits a
    // dangling external `bz$<Type>$init`/`$ctor` reference (the FINDING [2] link
    // failure) even if reached out-of-band. The bare calloc'd object is returned
    // as-is. Full cross-module generic construction needs the generic body in
    // metadata so the importer can monomorphize locally — a deferred feature.
    const bool importedGeneric =
        ctx_.isImported(t) && !t->genericParams.empty();
    if (ctx_.isImported(t) && !importedGeneric) {
        llvm::Function* initFn = declareImportedInit(t);
        builder_.CreateCall(initFn, {obj});
    } else if (!ctx_.isImported(t)) {
        emitInstanceInit(t, obj, st);
    }
    if (importedGeneric) {
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        return obj;  // decline: no dangling per-type symbol for a generic import
    }

    // CG10: run the explicit constructor (if any) after the field
    // initializers, passing the new instance as `this` plus the
    // lowered positional arguments. (Constructor overloads and
    // chained `base(...)`/`this(...)` calls are later slices — sema
    // defers the overloaded case too, so at most one ctor reaches us.)
    // CG16: select the ctor by arity (covers single-ctor and
    // arity-distinct overloads; full type-based overload resolution
    // is deferred). The chosen ctor runs its own base/this chain.
    //
    // §2.5 / §2.6 step 4 / §22.7 / §22.8 (issue #328 FINDING [7]): when `new`
    // targets an IMPORTED type, the constructor is defined in the linked
    // dependency, not this module — there is no AST body to inline and no
    // local ctorFns_ entry. Declare the external `bz$<Type>$ctor` and emit the
    // CALL (instance + positional args), so the library's ctor body runs
    // cross-module (manifestation A) instead of the field-zero bare calloc. The
    // ctor signature was reconstructed by the loader (manifestation B). Named /
    // modifier arguments are conservatively skipped here, matching the sema-side
    // check (cross-module ctor overloads + by-ref binding are deferred); an
    // imported type cannot be generic-substituted in this path (its struct is
    // built from reconstructed field types, not a live template).
    if (ctx_.isImported(t)) {
        bool allPositional = true;
        for (const auto& a : ne.args)
            if (!a.name.empty() || !a.modifier.empty() || a.isDiscard)
                { allPositional = false; break; }
        if (sema::ConstructorSymbol* cs = ctorForArgs(t, ne.args);
            cs && allPositional && ne.args.size() == cs->params.size()) {
            llvm::Function* ctorFn = declareImportedCtor(cs);
            std::vector<llvm::Value*> args;
            args.push_back(obj);
            bool ok = true;
            auto* fnTy = ctorFn->getFunctionType();
            for (std::size_t ai = 0; ai < ne.args.size(); ++ai) {
                if (!ne.args[ai].value) { ok = false; break; }
                llvm::Value* v = lowerExpr(*ne.args[ai].value);
                if (!v) { ok = false; break; }
                // §5.1: widen an int argument to a wider numeric parameter, as
                // the in-module ctor-call path does, so the ABI matches.
                if (ai + 1 < fnTy->getNumParams())
                    v = coerceNumericToType(
                        v, fnTy->getParamType(ai + 1),
                        isUnsignedTargetExpr(*ne.args[ai].value));
                args.push_back(v);
            }
            if (ok && args.size() == ctorFn->arg_size())
                builder_.CreateCall(ctorFn, args);
        }
        // §22.9 / §28.3 (FINDING [4]): an imported constructor may throw. As for
        // an in-module ctor, the imported ctor (compiled by this same compiler in
        // the dependency) disposes its assigned owned fields + base subobject on
        // its own unwind path without freeing `this`; reclaim the instance block
        // here, the allocating site, exactly once on the unwind edge.
        emitNewPendingCheck(obj);  // CG20: an imported constructor may throw
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        return obj;
    }

    // CG-generic: when the type has an active generic substitution, the
    // constructor function was compiled for the generic template (T→i32)
    // but the actual arguments use concrete types. Emit the constructor
    // body INLINE with the substitution active so that parameter types
    // and field stores use the correct concrete types. This avoids the
    // ABI mismatch that results from calling a T→i32 ctor with f64 args.
    if (sema::ConstructorSymbol* cs = ctorForArgs(t, ne.args)) {
        auto it = ctorFns_.find(cs);
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (it != ctorFns_.end() && cd) {
            std::vector<llvm::Value*> args;
            args.push_back(obj);
            if (buildOrderedArgs(cd->params, ne.args, args) &&
                args.size() == it->second->arg_size()) {
                // Check for type mismatch (generic substitution active).
                bool typesMismatch = false;
                auto* callFnTy = it->second->getFunctionType();
                for (std::size_t ai = 0; ai < args.size(); ++ai)
                    if (args[ai]->getType() != callFnTy->getParamType(ai))
                        { typesMismatch = true; break; }
                // CG-generic: even when all LLVM types match (e.g. ptr == ptr
                // for function-typed fields), the struct layout may differ
                // between the non-spec and spec versions.  Always inline
                // constructors for generic classes when a substitution is
                // active so GEPs use the spec struct layout.
                bool needsInlineForLayout = !genericSubst_.empty();

                if (!typesMismatch && !needsInlineForLayout) {
                    builder_.CreateCall(it->second, args);
                    // §22.9 / §28.3 (FINDING [4]): the called ctor disposed its
                    // assigned owned fields + base subobject on its own unwind
                    // path (without freeing `this`); reclaim the instance block
                    // here on the throw edge.
                    emitNewPendingCheck(obj);
                } else {
                    // Inline the constructor body with the generic
                    // substitution so field stores use concrete types.
                    //
                    // §22.9 / §28.3 (FINDING [4]): the body is inlined into THIS
                    // frame, so its exception edges route to this function's
                    // unwind chain — not a dedicated ctor cleanup block. Build a
                    // cleanup block that disposes the assigned owned fields + base
                    // subobject (with the generic substitution still active, so an
                    // owned type-parameter field is classified by its concrete
                    // type), frees the instance block, then continues to the
                    // ENCLOSING unwind. Route the inlined body's exceptions to it
                    // by setting unwindTarget_ = cleanupBB for the body (and
                    // unwindScopeDepth_ to the current dispose depth, so unwound
                    // owned locals of the body are disposed first). Saved/restored
                    // around the body. This mirrors emitConstructorBody's cleanup
                    // for the called-ctor case.
                    llvm::Value* savedThis = currentThis_;
                    auto savedLocals = locals_;
                    auto savedParams = paramSlots_;
                    llvm::BasicBlock* savedUnwindTarget = unwindTarget_;
                    std::size_t savedUnwindDepth = unwindScopeDepth_;
                    currentThis_ = obj;
                    locals_.clear();
                    paramSlots_.clear();
                    // Bind constructor params to the concrete arg values.
                    for (std::size_t pi = 0; pi < cs->params.size(); ++pi) {
                        auto* slot = entryAlloca(args[pi + 1]->getType());
                        builder_.CreateStore(args[pi + 1], slot);
                        paramSlots_[cs->params[pi]] = slot;
                    }
                    // Capture the enclosing unwind destination (BEFORE we
                    // override unwindTarget_) so the cleanup re-propagates there.
                    auto* savedInsert = builder_.GetInsertBlock();
                    llvm::BasicBlock* enclosingUnwind = unwindBB();
                    auto* cleanupBB = llvm::BasicBlock::Create(
                        llvm_, "ctor.inline.cleanup", currentFn_);
                    builder_.SetInsertPoint(cleanupBB);
                    emitOwnedFieldDisposals(t);
                    emitBaseDisposeChain(t);
                    builder_.CreateCall(freeFn(), {obj});
                    builder_.CreateBr(enclosingUnwind);
                    if (savedInsert) builder_.SetInsertPoint(savedInsert);
                    unwindTarget_ = cleanupBB;
                    unwindScopeDepth_ = disposeStack_.size();
                    lowerScopedBlock(cd->body);
                    unwindTarget_ = savedUnwindTarget;
                    unwindScopeDepth_ = savedUnwindDepth;
                    currentThis_ = savedThis;
                    locals_ = std::move(savedLocals);
                    paramSlots_ = std::move(savedParams);
                }
            }
        }
    } else if (!hasAnyConstructor(t)) {
        // CG16: no explicit ctor → the implicit ctor runs an implicit
        // `base()`, so a base-class ctor still runs on `new Derived()`.
        emitImplicitBase(t, obj);
        // §22.9 / §28.3 (FINDING [4]): the implicit base ctor may throw; the
        // base ctor's own cleanup disposed the base subobject without freeing
        // `this`, so reclaim the instance block here on the throw edge.
        emitNewPendingCheck(obj);  // CG20: the base constructor may throw
    }
    // CG-generic: record the substitution for this new object so that
    // lowerLocalDecl can associate it correctly even when the outer
    // savedSubst would override the lastCallGenericSubst_ from a prior call.
    if (!genericSubst_.empty())
        lastCallGenericSubst_ = genericSubst_;
    // Restore the outer generic substitution.
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    return obj;
}


// §5.1: coerce a lowered value to a target LLVM type along a §5.1-admitted
// numeric widening. Sema has already accepted the conversion (assignment to a
// wider local/field, mixed-type arithmetic per §6.2, or argument binding to a
// wider parameter per §22.6); codegen must materialize it, because the lowered
// value still carries its source LLVM type. Without this an `int` (i32) stored
// into / passed where a `float`/`double` is expected reaches the consumer as
// raw integer bits (issue #408). Only the widening directions §5.1 admits are
// applied; everything else (pointers, structs, equal types, narrowing) is left
// untouched, so callers can invoke this unconditionally.
llvm::Value* Emitter::coerceNumericToType(llvm::Value* v,
                                          llvm::Type* targetTy,
                                          bool srcUnsigned) {
    if (!v || !targetTy) return v;
    llvm::Type* srcTy = v->getType();
    if (srcTy == targetTy) return v;
    // int → float / double (§5.1: integers widen to binary floats). An
    // unsigned source (uint/ulong/ushort/byte/char) zero-interprets via
    // UIToFP; a signed source sign-interprets via SIToFP (issue #410).
    if (srcTy->isIntegerTy() && !srcTy->isIntegerTy(1) &&
        targetTy->isFloatingPointTy())
        return srcUnsigned ? builder_.CreateUIToFP(v, targetTy)
                           : builder_.CreateSIToFP(v, targetTy);
    // float → double (§5.1: the one binary-float widening).
    if (srcTy->isFloatingPointTy() && targetTy->isFloatingPointTy() &&
        srcTy->getPrimitiveSizeInBits() < targetTy->getPrimitiveSizeInBits())
        return builder_.CreateFPExt(v, targetTy);
    // integer widening (§5.1: e.g. int → long, uint → long). An unsigned
    // source zero-extends (ZExt); a signed source sign-extends (SExt)
    // (issue #410).
    if (srcTy->isIntegerTy() && !srcTy->isIntegerTy(1) &&
        targetTy->isIntegerTy() &&
        srcTy->getIntegerBitWidth() < targetTy->getIntegerBitWidth())
        return srcUnsigned ? builder_.CreateZExt(v, targetTy)
                           : builder_.CreateSExt(v, targetTy);
    return v;  // no §5.1 numeric widening applies — leave the value as is.
}


llvm::Value* Emitter::lowerConversion(sema::PrimitiveType target,
                             const ast::CallExpr& call) {
    using P = sema::PrimitiveType;
    if (call.args.size() != 1 || !call.args[0].value) return nullptr;
    const ast::Expr& argE = *call.args[0].value;
    // §5.2 / §9.6: string(x) converts any primitive to its string
    // representation.  Route through emitPrimitiveToString (which handles
    // int/long/double/float/bool/char) — the ordinary numeric-type paths
    // below would crash because stringType() is a struct, not an integer.
    if (target == P::String) {
        llvm::Value* src = lowerExpr(argE);
        if (!src) return nullptr;
        // Infer source primitive from sema type; default to Int.
        P srcPrim = P::Int;
        if (auto* rt = ctx_.exprType(argE))
            if (rt->kind == sema::TypeRef::Kind::Primitive)
                srcPrim = rt->primitive;
        // For unresolved generic T that resolved to ptr (e.g. T=string),
        // infer from the LLVM type: ptr → string, i32 → int, etc.
        if (srcPrim == P::Int && src->getType()->isPointerTy())
            srcPrim = P::String;
        // Already a string → identity.
        if (srcPrim == P::String) return src;
        return emitPrimitiveToString(src, srcPrim);
    }
    llvm::Value* src = lowerExpr(argE);
    if (!src) return nullptr;
    llvm::Type* dstTy = lowerPrimitive(target);
    llvm::Type* srcTy = src->getType();
    if (srcTy == dstTy) return src;
    bool dstFP = dstTy->isFloatingPointTy();
    bool srcFP = srcTy->isFloatingPointTy();
    bool dstUns = isUnsignedPrim(target);
    bool srcUns = isUnsignedTargetExpr(argE);
    if (srcFP && dstFP) {
        return srcTy->getPrimitiveSizeInBits() <
                       dstTy->getPrimitiveSizeInBits()
                   ? builder_.CreateFPExt(src, dstTy)
                   : builder_.CreateFPTrunc(src, dstTy);
    }
    if (srcFP)  // float → int
        return dstUns ? builder_.CreateFPToUI(src, dstTy)
                      : builder_.CreateFPToSI(src, dstTy);
    if (dstFP)  // int → float
        return srcUns ? builder_.CreateUIToFP(src, dstTy)
                      : builder_.CreateSIToFP(src, dstTy);
    // int → int.
    unsigned sb = srcTy->getIntegerBitWidth();
    unsigned db = dstTy->getIntegerBitWidth();
    if (db < sb) {
        // §5.2: integer narrowing raises OverflowException if the value
        // does not fit the target. Truncate, extend back with the
        // target's signedness, and compare to the original — a mismatch
        // means the value was out of range.
        llvm::Value* trunc = builder_.CreateTrunc(src, dstTy);
        llvm::Value* back = dstUns ? builder_.CreateZExt(trunc, srcTy)
                                   : builder_.CreateSExt(trunc, srcTy);
        emitGuardThrow(builder_.CreateICmpNE(back, src),
                       "OverflowException");
        return trunc;
    }
    if (db > sb) {
        // §5.1: bool (i1) widening to any integer always zero-extends
        // (true=1, false=0). Sign-extending i1 would give -1 for true.
        if (srcTy->isIntegerTy(1))
            return builder_.CreateZExt(src, dstTy);
        return srcUns ? builder_.CreateZExt(src, dstTy)
                      : builder_.CreateSExt(src, dstTy);
    }
    return src;  // same width, differing signedness (e.g. int ↔ uint)
}


llvm::Value* Emitter::lowerEnumCast(const ast::CallExpr& call) {
    if (call.args.size() != 1 || !call.args[0].value) return nullptr;
    llvm::Value* src = lowerExpr(*call.args[0].value);
    if (!src || !src->getType()->isIntegerTy()) return nullptr;
    unsigned sb = src->getType()->getIntegerBitWidth();
    if (sb < 32) return builder_.CreateSExt(src, i32());
    if (sb > 32) return builder_.CreateTrunc(src, i32());
    return src;
}


llvm::Value* Emitter::buildRecordValue(const sema::TypeSymbol* t,
                              const std::vector<ast::CallArgument>& args) {
    llvm::StructType* st = recordStruct(t);
    llvm::Value* slot = entryAlloca(st);
    // §9.1 / §9.2: storage = positional/body FieldSymbols + body
    // auto-properties (PropertySymbol), in declaration order.
    std::vector<const sema::Symbol*> fields;
    collectRecordStorage(t, fields);

    // §9.1 positional record: a bare positional argument list whose count
    // matches the storage members maps each argument to its field by
    // position. This is the original positional-record construction path,
    // taken only when the record has no explicit constructor.
    if (!hasAnyConstructor(t) && !args.empty() &&
        args.size() == fields.size()) {
        bool allPositional = true;
        for (const auto& a : args)
            if (!a.name.empty()) { allPositional = false; break; }
        if (allPositional) {
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (!args[i].value) return nullptr;
                llvm::Value* v = lowerExpr(*args[i].value);
                if (!v) return nullptr;
                // CG-nullable: a NULLABLE VALUE field (int?, bool?, …, and a
                // nullable enum — issue #364) stores the tagged struct {i8
                // hasValue; T value}, never a bare T. Wrap a plain value
                // argument (and lower a `null` literal to the all-zero null),
                // mirroring buildOrderedArgs — without this the bare T would be
                // stored into the wider struct slot, leaving the value half
                // uninitialized. per §9.1
                const sema::TypeRef* fty = recordMemberType(fields[i]);
                if (isNullableValueTy(fty)) {
                    auto* expectedSt = nullableValueStructTy(fty);
                    if (v->getType() != expectedSt) {
                        if (v->getType()->isPointerTy() &&
                            llvm::isa<llvm::ConstantPointerNull>(v))
                            v = nullableValueNull(fty);
                        else if (v->getType() == expectedSt->getElementType(1))
                            v = wrapNullableValue(fty, v);
                    }
                }
                auto idxIt = fieldIndex_.find(fields[i]);
                if (idxIt == fieldIndex_.end()) return nullptr;
                builder_.CreateStore(
                    v, builder_.CreateStructGEP(st, slot, idxIt->second));
            }
            return builder_.CreateLoad(st, slot);
        }
    }

    // §9.2 body record (or a record constructed through an explicit
    // constructor): zero the storage, run auto-property / field
    // initializers, then run the matching constructor with `this` bound to
    // the struct slot. Mirrors the class construction path in lowerNew but
    // for a by-value struct.
    builder_.CreateStore(llvm::ConstantAggregateZero::get(st), slot);
    for (const sema::Symbol* m : fields) {
        const ast::Expr* init = nullptr;
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
            init = fd->initializer.get();
        else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
            init = pd->autoInitializer.get();  // §9.2: `property X := v;`
        if (!init) continue;
        auto idxIt = fieldIndex_.find(m);
        if (idxIt == fieldIndex_.end()) continue;
        llvm::Value* v = lowerExpr(*init);
        if (!v) continue;
        // CG-nullable: a NULLABLE VALUE field initialized with a plain value
        // (`property N: int? := 9;`, or `property C: Color? := Color.Green;` —
        // issue #364) must wrap the bare value into the {i8 hasValue; T value}
        // struct, like the positional path above. per §9.2
        const sema::TypeRef* fty = recordMemberType(m);
        if (isNullableValueTy(fty)) {
            auto* expectedSt = nullableValueStructTy(fty);
            if (v->getType() != expectedSt) {
                if (v->getType()->isPointerTy() &&
                    llvm::isa<llvm::ConstantPointerNull>(v))
                    v = nullableValueNull(fty);
                else if (v->getType() == expectedSt->getElementType(1))
                    v = wrapNullableValue(fty, v);
            }
        }
        builder_.CreateStore(
            v, builder_.CreateStructGEP(st, slot, idxIt->second));
    }

    if (sema::ConstructorSymbol* cs = ctorForArgs(t, args)) {
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) return nullptr;
        // Inline the constructor body with `this` = the record slot pointer
        // (record methods access fields through a struct pointer, §9.4) and
        // the parameters bound to fresh slots.
        std::vector<llvm::Value*> ordered;
        ordered.push_back(slot);  // placeholder for the implicit receiver
        if (!buildOrderedArgs(cd->params, args, ordered)) return nullptr;
        llvm::Value* savedThis = currentThis_;
        auto savedLocals = locals_;
        auto savedParams = paramSlots_;
        currentThis_ = slot;
        locals_.clear();
        paramSlots_.clear();
        for (std::size_t pi = 0; pi < cs->params.size(); ++pi) {
            llvm::Value* av = ordered[pi + 1];
            auto* pslot = entryAlloca(av->getType());
            builder_.CreateStore(av, pslot);
            paramSlots_[cs->params[pi]] = pslot;
        }
        lowerScopedBlock(cd->body);
        currentThis_ = savedThis;
        locals_ = std::move(savedLocals);
        paramSlots_ = std::move(savedParams);
        emitPendingCheck();  // §28: a constructor may throw
    } else if (!args.empty()) {
        // Arguments were supplied but no constructor accepts them — fall
        // back to a failure so the caller does not silently drop them.
        return nullptr;
    }
    return builder_.CreateLoad(st, slot);  // the record value (by value)
}


llvm::Value* Emitter::lowerCaseExpr(const ast::CaseExpr& ce) {
    if (!ce.scrutinee) return nullptr;  // scrutinee-less form deferred
    llvm::Value* scrut = lowerExpr(*ce.scrutinee);
    if (!scrut) return nullptr;
    const sema::TypeSymbol* enumTy = enumTypeOf(*ce.scrutinee);
    bool isStr = isStringExprType(*ce.scrutinee);
    // §7 / issue #447: pass the scrutinee's resolved TypeRef so patternMatches
    // can detect a nullable VALUE type ({i8 hasValue; T value} struct) and
    // apply hasValue-based matching instead of the pointer-based paths.
    const sema::TypeRef* scrutTy = ctx_.exprType(*ce.scrutinee);
    llvm::Type* resultTy = i32();
    if (auto* rt = ctx_.exprType(ce)) resultTy = lowerTypeRef(rt);

    auto* endBB = llvm::BasicBlock::Create(llvm_, "case.end", currentFn_);
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incomings;

    for (const auto& arm : ce.arms) {
        if (arm.bodyKind != ast::CaseArmBodyKind::Expression ||
            !arm.exprBody)
            return nullptr;  // block-body arms deferred
        llvm::Value* pm = patternMatches(arm.pattern.get(), scrut, enumTy,
                                         isStr, scrutTy);
        if (!pm) return nullptr;
        auto* bodyBB =
            llvm::BasicBlock::Create(llvm_, "case.arm", currentFn_);
        auto* nextBB =
            llvm::BasicBlock::Create(llvm_, "case.next", currentFn_);
        // CG28b: a BindingPattern arm exposes the scrutinee value via a
        // local slot keyed on the BindingPattern AST node — matches the
        // sema decl anchor so localSlot() resolves the bound identifier.
        // The slot must be filled before the guard (which may reference the
        // bound name) and before the arm body. The alloca goes in the
        // function entry block; the store is emitted in the current
        // (pattern-test) block, before we branch to the guard or body.
        if (auto* bp = dynamic_cast<const ast::BindingPattern*>(
                           arm.pattern.get())) {
            llvm::AllocaInst* slot = entryAlloca(scrut->getType());
            builder_.CreateStore(scrut, slot);
            locals_[bp] = slot;
        }
        if (arm.guard) {
            auto* guardBB =
                llvm::BasicBlock::Create(llvm_, "case.guard", currentFn_);
            builder_.CreateCondBr(pm, guardBB, nextBB);
            builder_.SetInsertPoint(guardBB);
            llvm::Value* g = lowerExpr(*arm.guard);
            if (!g) return nullptr;
            builder_.CreateCondBr(g, bodyBB, nextBB);
        } else {
            builder_.CreateCondBr(pm, bodyBB, nextBB);
        }
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* v = lowerExpr(*arm.exprBody);
        // CG40 (§17.12 / §19): an arm body that diverges (`throw E`, or a
        // body that branches to the unwind/propagate target) has already
        // terminated its block via the throw — mirroring lowerConditional's
        // per-branch diverged_ handling. Such an arm never reaches endBB, so
        // it is excluded from the result PHI and emits no `br` to endBB
        // (which would give the block two terminators). A non-diverging arm
        // is coerced to the case's §20 common result type IN THIS arm's block
        // (so the coerced value dominates its PHI edge) before joining; this
        // is what makes a numeric-widening case (`5` int vs `9L` long, common
        // `long`) build an i64 PHI instead of crashing on an i32/i64
        // mismatch — the same fix lowerConditional applies to `if` branches.
        bool armDiv = diverged_;
        if (!armDiv) {
            if (!v) return nullptr;
            if (resultTy)
                v = coerceNumericToType(v, resultTy,
                                        isUnsignedTargetExpr(*arm.exprBody));
            incomings.push_back({v, builder_.GetInsertBlock()});
            builder_.CreateBr(endBB);
        }
        // Reset for the next arm's pattern-test / body: a prior arm's
        // divergence must not bleed into the next arm.
        diverged_ = false;
        builder_.SetInsertPoint(nextBB);
    }
    // No arm matched → MatchFailureException (§19.6).
    emitThrowBuiltin("MatchFailureException");

    builder_.SetInsertPoint(endBB);
    // CG40: if EVERY arm diverged, no arm reaches endBB — the whole case
    // diverges (like a conditional whose both branches throw). Report it via
    // diverged_ so the enclosing return/decl/expr context skips emitting code
    // into the (predecessorless) endBB, exactly as lowerConditional does.
    if (incomings.empty()) {
        // endBB has no predecessors and the caller (diverged_) emits nothing
        // into it; an empty block is malformed IR (no terminator). Cap it with
        // `unreachable` so the block is well-formed — it is provably dead.
        builder_.CreateUnreachable();
        diverged_ = true;
        return llvm::PoisonValue::get(resultTy);
    }
    auto* phi = builder_.CreatePHI(incomings[0].first->getType(),
                                   incomings.size());
    for (auto& [v, bb] : incomings) phi->addIncoming(v, bb);
    return phi;
}


llvm::Value* Emitter::patternMatches(const ast::Pattern* pat, llvm::Value* scrut,
                            const sema::TypeSymbol* enumTy,
                            bool isStr,
                            const sema::TypeRef* scrutTy) {
    if (dynamic_cast<const ast::DiscardPattern*>(pat))
        return builder_.getInt1(true);

    // §7 / §19 / issues #447 + #448: a nullable VALUE type scrutinee is a
    // tagged {i8 hasValue; T value} struct, NOT a pointer. All pointer-based
    // pattern tests (null-check via ConstantPointerNull, emitEquals on the
    // full struct) produce illegal IR on this operand. Instead extract the two
    // fields and route each pattern through hasValue-based tests:
    //   NullPattern      → !hasValue (the null slot is absent)
    //   ConstantPattern  → hasValue AND value == const
    //   NamedPattern     → hasValue AND innerValue == enumConst
    //   DiscardPattern   → true (handled above, no change)
    // The non-nullable path is taken for all other scrutinee kinds.
    bool isNullableVal = scrutTy && isNullableValueTy(scrutTy) &&
                         scrut->getType()->isStructTy();
    if (isNullableVal) {
        llvm::Value* hasVal = nullablePrimHasValue(scrut);  // i1
        llvm::Value* innerVal = nullablePrimValue(scrut);   // the T field

        // §19: NullPattern matches when hasValue == 0.
        if (dynamic_cast<const ast::NullPattern*>(pat))
            return builder_.CreateNot(hasVal, "null.match");

        // §19.8: NamedPattern on a nullable enum — hasValue AND enum == const.
        if (auto* np = dynamic_cast<const ast::NamedPattern*>(pat)) {
            if (enumTy && np->name && !np->name->parts.empty()) {
                if (llvm::Value* cv =
                        enumConstantByName(enumTy, np->name->parts.back())) {
                    llvm::Value* eq = emitEquals(innerVal, cv, /*isStr=*/false);
                    return builder_.CreateAnd(hasVal, eq, "nv.enum.match");
                }
            }
            return nullptr;  // type-test pattern deferred
        }

        // §19: ConstantPattern on a nullable value — hasValue AND value == c.
        if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(pat)) {
            llvm::Value* any = builder_.getInt1(false);
            for (const auto& c : cp->constants) {
                if (!c) continue;
                llvm::Value* cv = lowerExpr(*c);
                if (!cv) cv = enumConstantOfPatternExpr(*c, enumTy);
                if (!cv) return nullptr;
                llvm::Value* eq = emitEquals(innerVal, cv, /*isStr=*/false);
                llvm::Value* match = builder_.CreateAnd(hasVal, eq, "nv.val.match");
                any = builder_.CreateOr(any, match);
            }
            return any;
        }

        // Other pattern kinds on a nullable value are deferred.
        return nullptr;
    }

    // §19.5: a BindingPattern's match condition depends on whether it is
    // typed. An unconstrained `var x` always matches; a typed `var x: T`
    // is a type test (`scrut is-a T`) and must NOT match a value of an
    // unrelated type. The typed-vs-unconstrained logic lives in the
    // BindingPattern handler below (it needs `bp->type`); do not short-
    // circuit to `true` here, or `var c: Circle` would wrongly match a
    // non-Circle scrutinee and the arm's member accesses would read it as
    // a Circle. (Slot registration still happens in the arm-body preamble.)
    // CG43: a bare qualified name `E.Red` parses as a NamedPattern; when
    // the scrutinee is an enum, resolve it to that enum's constant and
    // test equality. (A name denoting a type — a type-test pattern — is
    // still deferred.)
    if (auto* np = dynamic_cast<const ast::NamedPattern*>(pat)) {
        if (enumTy && np->name && !np->name->parts.empty()) {
            if (llvm::Value* cv =
                    enumConstantByName(enumTy, np->name->parts.back()))
                return emitEquals(scrut, cv, isStr);
        }
        return nullptr;  // type-test pattern deferred
    }
    if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(pat)) {
        llvm::Value* any = builder_.getInt1(false);
        for (const auto& c : cp->constants) {
            if (!c) continue;
            llvm::Value* cv = lowerExpr(*c);
            // CG43: an enum-constant alternative (`E.Red`, `E.Green`) has
            // no resolved symbol on the pattern expression, so lowerExpr
            // misses; resolve it by name against the scrutinee's enum.
            if (!cv) cv = enumConstantOfPatternExpr(*c, enumTy);
            if (!cv) return nullptr;
            // CG27b: string scrutinee → content comparison (not ptr cmp).
            any = builder_.CreateOr(any, emitEquals(scrut, cv, isStr));
        }
        return any;
    }
    if (auto* rp = dynamic_cast<const ast::RelOpPattern*>(pat)) {
        llvm::Value* rv = rp->expr ? lowerExpr(*rp->expr) : nullptr;
        if (!rv) return nullptr;
        bool fp = scrut->getType()->isFloatingPointTy();
        if (rp->op == "<")
            return fp ? builder_.CreateFCmpOLT(scrut, rv)
                      : builder_.CreateICmpSLT(scrut, rv);
        if (rp->op == ">")
            return fp ? builder_.CreateFCmpOGT(scrut, rv)
                      : builder_.CreateICmpSGT(scrut, rv);
        if (rp->op == "<=")
            return fp ? builder_.CreateFCmpOLE(scrut, rv)
                      : builder_.CreateICmpSLE(scrut, rv);
        if (rp->op == ">=")
            return fp ? builder_.CreateFCmpOGE(scrut, rv)
                      : builder_.CreateICmpSGE(scrut, rv);
    }
    // §19: range patterns `lo..hi` and `lo..<hi` — same semantics as
    // the `in` operator but in a case arm. The pattern matches when
    // lo <= scrut <= hi (inclusive) or lo <= scrut < hi (half-open).
    if (auto* rp = dynamic_cast<const ast::RangePattern*>(pat)) {
        auto* range = dynamic_cast<const ast::RangeExpr*>(
            rp->rangeExpr.get());
        if (range && range->lo && range->hi && !range->step) {
            llvm::Value* lo = lowerExpr(*range->lo);
            llvm::Value* hi = lowerExpr(*range->hi);
            if (!lo || !hi) return nullptr;
            bool fp = scrut->getType()->isFloatingPointTy();
            // §19.3: range patterns — use FP comparisons for float scrutinees.
            llvm::Value* geL = fp ? builder_.CreateFCmpOGE(scrut, lo)
                                  : builder_.CreateICmpSGE(scrut, lo);
            llvm::Value* leH = fp
                ? (range->inclusive ? builder_.CreateFCmpOLE(scrut, hi)
                                    : builder_.CreateFCmpOLT(scrut, hi))
                : (range->inclusive ? builder_.CreateICmpSLE(scrut, hi)
                                    : builder_.CreateICmpSLT(scrut, hi));
            return builder_.CreateAnd(geL, leH);
        }
    }
    // §19: BindingPattern `var x` or `var x: T` (§3016).
    // Unconstrained `var x` always matches and binds the scrutinee's value
    // to `x` within the arm body. The binding is a borrow (non-destructive).
    // `var x: T` with a type annotation is a type-test + binding.
    if (auto* bp = dynamic_cast<const ast::BindingPattern*>(pat)) {
        // Look up the LocalSymbol for this binding from sema.
        // Sema symbolizes binding-pattern names in the arm scope.
        // We need to make the scrutinee value available as `bp->name`.
        // For now, store into the arm's local symbol slot if we can find it.
        // The alloca for the binding variable is created in lowerCaseExpr/Stmt
        // by looking up the arm's symbols — but currently we don't thread that
        // context here. Simplest: always match (true) and let the arm body
        // resolve the name directly through the scrutinee slot.
        // TODO: wire binding symbol → alloca and store scrut into it.
        // For the type-constrained form (`var x: T`), emit a type test:
        if (bp->type) {
            // Type-test pattern: matches if scrutinee is-a T.
            if (auto* rt = ctx_.resolvedType(*bp->type)) {
                if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration) {
                    auto dit = descArray_.find(rt->declaration);
                    if (dit != descArray_.end())
                        return emitIsA(scrut, dit->second);
                }
            }
            return nullptr;  // type not found — deferred
        }
        // Unconstrained `var x`: always matches.
        return builder_.getInt1(true);
    }
    // §19: NullPattern — matches null.
    if (dynamic_cast<const ast::NullPattern*>(pat)) {
        return builder_.CreateICmpEQ(
            scrut, llvm::ConstantPointerNull::get(ptrTy()), "null.match");
    }
    // §19 / §9.4: DestructuringPattern `TypeName(x, y, ...)` — for record
    // value types the type test is satisfied statically (the scrutinee's
    // static type already determines which record it is). Extract each
    // positional field from the struct value and store it in a local alloca
    // so the arm body (and guard) can read the bound names.  The alloca is
    // keyed on the PositionalPattern's address, mirroring the sema anchor
    // (`lsym->decl = reinterpret_cast<const ast::Node*>(&sp)`) so that
    // `localSlot()` resolves the identifier to the right slot.
    if (auto* dp = dynamic_cast<const ast::DestructuringPattern*>(pat)) {
        if (!dp->typeName) return nullptr;
        // Find the record TypeSymbol by the pattern's last name part.
        std::string_view lastName = dp->typeName->parts.empty()
            ? std::string_view{} : dp->typeName->parts.back();
        const sema::TypeSymbol* recTy = nullptr;
        for (const auto& sym : ctx_.symbols()) {
            if (auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get())) {
                if (ts->name == lastName &&
                    ts->kind == ast::TypeDeclKind::Record) {
                    recTy = ts; break;
                }
            }
        }
        if (!recTy) return nullptr;
        // Ensure the record struct (and fieldIndex_) is populated.
        recordStruct(recTy);
        // Collect positional (non-static) fields in declaration order.
        std::vector<const sema::FieldSymbol*> fields;
        for (const sema::Symbol* m : recTy->members) {
            if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m)) {
                if (!isStaticField(fs)) fields.push_back(fs);
            }
        }
        // Extract each field value from the struct and register the slot.
        // Build up a cumulative match condition for constant sub-patterns.
        llvm::Value* matched = builder_.getInt1(true);
        for (std::size_t i = 0; i < dp->subPatterns.size(); ++i) {
            const auto& sp = dp->subPatterns[i];
            if (i >= fields.size()) return nullptr;  // arity mismatch
            auto idxIt = fieldIndex_.find(fields[i]);
            if (idxIt == fieldIndex_.end()) return nullptr;
            llvm::Value* fieldVal =
                builder_.CreateExtractValue(scrut, {idxIt->second});
            if (!sp.nakedBinding.empty()) {
                // Binding: store field value into a slot for `sp.nakedBinding`.
                llvm::AllocaInst* slot = entryAlloca(fieldVal->getType());
                builder_.CreateStore(fieldVal, slot);
                const auto* key = reinterpret_cast<const ast::Node*>(&sp);
                locals_[key] = slot;
            } else if (sp.pattern) {
                // Non-binding sub-pattern: check it against the field value.
                // For constant patterns (e.g. `0`), emit an equality check.
                llvm::Value* subMatch =
                    patternMatches(sp.pattern.get(), fieldVal);
                if (subMatch)
                    matched = builder_.CreateAnd(matched, subMatch);
            }
        }
        // For a record scrutinee, the type test is satisfied if all
        // non-binding sub-pattern checks pass.
        return matched;
    }
    return nullptr;  // positional — deferred
}


llvm::Value* Emitter::emitEquals(llvm::Value* a, llvm::Value* b,
                        bool isStr) {
    if (isStr) return emitStringEq(a, b);
    return a->getType()->isFloatingPointTy()
               ? builder_.CreateFCmpOEQ(a, b)
               : builder_.CreateICmpEQ(a, b);
}


llvm::Value* Emitter::lowerConditional(const ast::ConditionalExpr& c) {
    if (!c.condition || !c.thenValue || !c.elseValue) return nullptr;
    llvm::Value* cond = lowerExpr(*c.condition);
    if (!cond) return nullptr;

    // §20 / §5.1: the conditional's result type is the §20 common type of
    // its branches, which sema records on the expression. The branches may
    // have different (but common-typed) source types — e.g. `5 if c else
    // 9L` is an int branch and a long branch whose common type is `long`.
    // A PHI requires every incoming value to share the PHI's type, so each
    // branch value is coerced to this result type before the join (in the
    // branch's own block, so the coerced value dominates its PHI edge).
    // Without this the i32/i64 mismatch crashes LLVM's PHI verifier.
    // Same-type branches pass through coerceNumericToType unchanged.
    llvm::Type* resultTy = nullptr;
    if (const sema::TypeRef* rt = ctx_.exprType(c)) resultTy = lowerTypeRef(rt);

    auto* thenBB = llvm::BasicBlock::Create(llvm_, "cond.then", currentFn_);
    auto* elseBB = llvm::BasicBlock::Create(llvm_, "cond.else", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "cond.end", currentFn_);
    builder_.CreateCondBr(cond, thenBB, elseBB);

    builder_.SetInsertPoint(thenBB);
    llvm::Value* tv = lowerExpr(*c.thenValue);
    bool thenDiv = diverged_;  // CG40: e.g. `cond ? throw E : b`
    if (tv && resultTy && !thenDiv)
        tv = coerceNumericToType(tv, resultTy,
                                 isUnsignedTargetExpr(*c.thenValue));
    llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
    if (!thenDiv) builder_.CreateBr(endBB);

    builder_.SetInsertPoint(elseBB);
    llvm::Value* ev = lowerExpr(*c.elseValue);
    bool elseDiv = diverged_;
    if (ev && resultTy && !elseDiv)
        ev = coerceNumericToType(ev, resultTy,
                                 isUnsignedTargetExpr(*c.elseValue));
    llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();
    if (!elseDiv) builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    if (!tv || !ev) return nullptr;
    // CG40: a branch that diverged (threw) does not reach endBB, so it
    // is excluded from the join. When only one branch reaches, its
    // value is the result; when both diverge, endBB is unreachable.
    diverged_ = thenDiv && elseDiv;
    if (thenDiv && elseDiv) {
        // Both branches threw: endBB has no predecessors and the caller emits
        // nothing into it (diverged_). Cap it with `unreachable` so the block
        // is a well-formed (provably dead) block rather than terminator-less.
        builder_.CreateUnreachable();
        return llvm::PoisonValue::get(tv->getType());
    }
    if (thenDiv) return ev;
    if (elseDiv) return tv;
    // Both branches reach the join: build the PHI over their (now
    // common-typed) values.
    llvm::PHINode* phi = builder_.CreatePHI(tv->getType(), 2);
    phi->addIncoming(tv, thenEnd);
    phi->addIncoming(ev, elseEnd);
    return phi;
}


}  // namespace bezel::codegen
