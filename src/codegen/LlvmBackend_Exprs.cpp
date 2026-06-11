#include "codegen/LlvmBackend_impl.hpp"

// §4.4 fix #521: decimal literal compile-time parse.
// bz_decimal_parse_cstr is compiled into the same process (the runtime is linked
// into bezel_tests and also available in the driver binary).  Calling it from the
// compiler during code generation is safe for both AOT and JIT modes.
extern "C" void bz_decimal_parse_cstr(const char* s, void* out);

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
    // §4.4 fix #521: decimal literal (m/M suffix) → parse at compile time
    // into a { i32 lo32, i32 mid32, i32 hi32, i32 flags } constant aggregate.
    if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
        bool isDecimal = false;
        if (auto* rt = ctx_.exprType(e))
            if (rt->kind == sema::TypeRef::Kind::Primitive &&
                rt->primitive == sema::PrimitiveType::Decimal)
                isDecimal = true;
        if (isDecimal) {
            // Parse the decimal literal at compile time via the runtime's
            // bz_decimal_parse_cstr, which is available as a plain C function.
            // Strip the trailing m/M suffix before passing to the parser.
            std::string digits;
            for (char c : fl->text) {
                if (c == '_' || c == 'm' || c == 'M') continue;
                digits.push_back(c);
            }
            // Call the runtime parse function directly (declared at top of file).
            uint32_t dec[4] = {0, 0, 0, 0};
            bz_decimal_parse_cstr(digits.c_str(), dec);
            llvm::StructType* dst = decimalStructType();
            return llvm::ConstantStruct::get(dst, {
                llvm::ConstantInt::get(i32(), dec[0]),
                llvm::ConstantInt::get(i32(), dec[1]),
                llvm::ConstantInt::get(i32(), dec[2]),
                llvm::ConstantInt::get(i32(), dec[3])
            });
        }
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
    // fix #499 / §9.8: RangeExpr in value position (e.g. `var r := a..b`).
    // Builds a Range struct { i64 start, i64 end, i64 step, i8 flags } so the
    // range can be stored in a local variable and used via member access / `in`.
    // The inline-literal fast paths (`x in lo..hi`, `for var i in lo..hi`) are
    // handled elsewhere and bypass this; this path only fires when the RangeExpr
    // is lowered as a general expression (initializer of a local, etc.).
    if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
        auto* sty = rangeStructType();
        // start: lo value widened to i64; 0 if lo is absent (open-ended low).
        llvm::Value* startV = builder_.getInt64(0);
        if (re->lo) {
            llvm::Value* lo = lowerExpr(*re->lo);
            if (!lo) return nullptr;
            startV = lo->getType() == i64()
                         ? lo
                         : builder_.CreateSExt(lo, i64(), "range.start");
        }
        // end: hi value widened to i64; 0 if hi is absent (open-ended high).
        llvm::Value* endV = builder_.getInt64(0);
        if (re->hi) {
            llvm::Value* hi = lowerExpr(*re->hi);
            if (!hi) return nullptr;
            endV = hi->getType() == i64()
                       ? hi
                       : builder_.CreateSExt(hi, i64(), "range.end");
        }
        // step: explicit step if present, else 1 (§16.4 default).
        llvm::Value* stepV = builder_.getInt64(1);
        if (re->step) {
            llvm::Value* s = lowerExpr(*re->step);
            if (!s) return nullptr;
            stepV = s->getType() == i64()
                        ? s
                        : builder_.CreateSExt(s, i64(), "range.step");
        }
        // flags: bit 0 = IsEndInclusive; bit 1 = startOpen; bit 2 = endOpen.
        uint8_t flags = 0;
        if (re->inclusive) flags |= 1u;
        if (!re->lo)       flags |= 2u;
        if (!re->hi)       flags |= 4u;
        llvm::Value* flagsV =
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), flags);
        // Assemble the struct via insertvalue.
        llvm::Value* sv = llvm::UndefValue::get(sty);
        sv = builder_.CreateInsertValue(sv, startV, {0u}, "range.0");
        sv = builder_.CreateInsertValue(sv, endV,   {1u}, "range.1");
        sv = builder_.CreateInsertValue(sv, stepV,  {2u}, "range.2");
        sv = builder_.CreateInsertValue(sv, flagsV, {3u}, "range.3");
        return sv;
    }
    // §17.10 (#487): typeof(T) → Type.  The `Type` runtime class (stdlib §2.13)
    // provides type identity and lattice metadata.  Its runtime representation
    // (a global pointer to a type-descriptor object) is not yet emitted by the
    // compiler backend (deferred alongside the broader System.Reflection surface,
    // stdlib §11).  Until that milestone, typeof(T) in a codegen-bound context
    // emits a clean compile-time error rather than reaching LLVM with an untyped
    // expression and producing silent garbage or a crash (#487).
    if (auto* to = dynamic_cast<const ast::TypeOfExpr*>(&e)) {
        (void)to;
        ctx_.diagnostics().report(
            Severity::Error, e.span.begin,
            "`typeof` is not yet implemented in codegen: the runtime `Type` "
            "class (stdlib \xC2\xA7""2.13) requires a type-descriptor table that is "
            "not yet emitted (\xC2\xA7""17.10, #487). This expression will be "
            "supported in a future milestone.");
        return llvm::PoisonValue::get(ptrTy());
    }
    // §17.9: nameof(QualifiedName) → compile-time string constant equal to
    // the rightmost identifier of the operand (the only segment for a simple
    // name, the last segment for a dotted name such as BankAccount.Deposit →
    // "Deposit"). The QualifiedName is stored verbatim in the AST; no runtime
    // resolution is needed — we wrap the lexeme in quotes and emit it as a
    // string literal global via lowerStringLiteral.
    if (auto* no = dynamic_cast<const ast::NameOfExpr*>(&e)) {
        std::string_view ident =
            (no->name && !no->name->parts.empty())
                ? no->name->parts.back()
                : std::string_view{};
        // Synthesise a quoted string that processStringText will accept.
        std::string quoted = "\"";
        quoted += ident;
        quoted += "\"";
        return lowerStringLiteral(quoted, /*verbatim=*/false);
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
                // issue #604 / §14.1 / §22.4: a NULLABLE BARE TYPE-PARAMETER
                // (`TFirst?` where TFirst is a class generic param) lowers to
                // the {i8, T} nullable struct in a monomorphized body, but
                // isNullableValueTy returns false for it (the TypeRef has
                // kind=Named, nullable=true, no declaration — it looks like an
                // unresolved type, not a concrete value type). Without this
                // guard the `!` operator falls through and returns the entire
                // {i8, T} struct instead of extracting the value field, so a
                // `GetFirst(): TFirst` body `return _first!` emits a
                // `ret { i8, i32 } %2` against an `i32` function → IR-verify
                // failure. Fix: when the operand TypeRef is a nullable bare
                // type-param, resolve it through genericSubstSemaTy_ to its
                // concrete sema type (nullableTypeParamConcrete does this),
                // and if that concrete nullable IS a nullable value type,
                // treat it the same as the direct isNullableValueTy case.
                if (rt->nullable &&
                    rt->kind == sema::TypeRef::Kind::Named &&
                    !rt->declaration && !genericSubstSemaTy_.empty()) {
                    sema::TypeRef concNullable;
                    if (nullableTypeParamConcrete(rt, concNullable) &&
                        isNullableValueTy(&concNullable)) {
                        llvm::Value* hasVal = nullablePrimHasValue(v);
                        emitGuardThrow(builder_.CreateNot(hasVal),
                                       "NullReferenceException");
                        return nullablePrimValue(v);
                    }
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
                    // fix #499 / §9.8: Range<T>.Contains(x) inside the
                    // CallExpr dispatch path (ma->target has Range type).
                    if (isRangeType(tt) && ma->member == "Contains" &&
                        call->args.size() == 1 && call->args[0].value) {
                        llvm::Value* rv = lowerExpr(*ma->target);
                        llvm::Value* x  = lowerExpr(*call->args[0].value);
                        if (rv && x) {
                            llvm::Value* lo  = builder_.CreateExtractValue(rv, {0u}, "rc.lo");
                            llvm::Value* hi  = builder_.CreateExtractValue(rv, {1u}, "rc.hi");
                            llvm::Value* flg = builder_.CreateExtractValue(rv, {3u}, "rc.flg");
                            llvm::Value* xW  = x->getType() == i64()
                                                   ? x
                                                   : builder_.CreateSExt(x, i64(), "rc.xw");
                            llvm::Value* geL  = builder_.CreateICmpSGE(xW, lo, "rc.geL");
                            llvm::Value* incl = builder_.CreateTrunc(flg, i1(), "rc.incl");
                            llvm::Value* leLE = builder_.CreateICmpSLE(xW, hi, "rc.leLE");
                            llvm::Value* leLT = builder_.CreateICmpSLT(xW, hi, "rc.leLT");
                            llvm::Value* leH  = builder_.CreateSelect(incl, leLE, leLT, "rc.leH");
                            return builder_.CreateAnd(geL, leH, "rc.res");
                        }
                    }
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
                        // §6.3 / stdlib §9.2: FP predicates as instance methods
                        // (x.IsNaN(), x.IsInfinity(), x.IsPositiveInfinity(),
                        //  x.IsNegativeInfinity(), x.IsFinite()).
                        // All produce i1 directly via inline fcmp sequences.
                        if (call->args.empty()) {
                            using P = sema::PrimitiveType;
                            bool isFP = tt->primitive == P::Double || tt->primitive == P::Float;
                            if (ma->member == "IsNaN" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                // fcmp uno %x, %x — true iff x is NaN (per §9.2).
                                return builder_.CreateFCmpUNO(recv, recv, "isnan");
                            }
                            if (ma->member == "IsInfinity" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                auto* fty = recv->getType();
                                auto* posInf = llvm::ConstantFP::getInfinity(fty, false);
                                auto* negInf = llvm::ConstantFP::getInfinity(fty, true);
                                return builder_.CreateOr(
                                    builder_.CreateFCmpOEQ(recv, posInf),
                                    builder_.CreateFCmpOEQ(recv, negInf));
                            }
                            // stdlib §9.2: IsPositiveInfinity — fcmp oeq %x, +inf.
                            if (ma->member == "IsPositiveInfinity" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                auto* posInf = llvm::ConstantFP::getInfinity(recv->getType(), false);
                                return builder_.CreateFCmpOEQ(recv, posInf, "isposinf");
                            }
                            // stdlib §9.2: IsNegativeInfinity — fcmp oeq %x, -inf.
                            if (ma->member == "IsNegativeInfinity" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                auto* negInf = llvm::ConstantFP::getInfinity(recv->getType(), true);
                                return builder_.CreateFCmpOEQ(recv, negInf, "isneginf");
                            }
                            if (ma->member == "IsFinite" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                // IsFinite = !IsNaN && !IsInfinity (per §9.2).
                                auto* fty = recv->getType();
                                llvm::Value* isNaN = builder_.CreateFCmpUNO(recv, recv);
                                llvm::Value* isInf = builder_.CreateOr(
                                    builder_.CreateFCmpOEQ(recv, llvm::ConstantFP::getInfinity(fty, false)),
                                    builder_.CreateFCmpOEQ(recv, llvm::ConstantFP::getInfinity(fty, true)));
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
                        // fix #586 / §2.8 / stdlib §9.1–§9.5: CompareTo(other: T): int
                        // — IComparable<T> contract for numeric and char primitives.
                        // Not string (handled by tryLowerStringMethod above) and not
                        // bool (which does not implement IComparable, per §9.4).
                        // Returns: negative if self < other, 0 if equal, positive if
                        // self > other.  Formula: (a > b) - (a < b) → -1, 0, or 1.
                        // Unsigned types use unsigned comparison (no negation needed;
                        // the result int is still a signed i32).
                        // Floating-point uses ordered (quiet-NaN-aware) comparisons
                        // per §9.2; NaN < everything in the ordered sense (fcmp olt).
                        if (call->args.size() == 1 && call->args[0].value &&
                            ma->member == "CompareTo") {
                            using P = sema::PrimitiveType;
                            P pt = tt->primitive;
                            bool isSignedInt   = pt == P::Int   || pt == P::Long  ||
                                                 pt == P::Short || pt == P::SByte ||
                                                 pt == P::Char;
                            bool isUnsignedInt = pt == P::UInt  || pt == P::ULong ||
                                                 pt == P::UShort|| pt == P::Byte;
                            bool isFP          = pt == P::Double|| pt == P::Float;
                            if (isSignedInt || isUnsignedInt || isFP) {
                                llvm::Value* lhs = lowerExpr(*ma->target);
                                llvm::Value* rhs = lowerExpr(*call->args[0].value);
                                if (!lhs || !rhs) return nullptr;
                                // Widen both sides to a uniform LLVM type if needed
                                // (e.g. char is i32 code-point in codegen).
                                if (lhs->getType() != rhs->getType()) {
                                    if (isFP) {
                                        // Widen float to double.
                                        auto* dty = llvm::Type::getDoubleTy(llvm_);
                                        lhs = builder_.CreateFPExt(lhs, dty, "cmp.ldw");
                                        rhs = builder_.CreateFPExt(rhs, dty, "cmp.rdw");
                                    } else {
                                        // Integer mismatch: widen narrower side.
                                        unsigned lb = lhs->getType()->getIntegerBitWidth();
                                        unsigned rb = rhs->getType()->getIntegerBitWidth();
                                        if (lb < rb) {
                                            lhs = isUnsignedInt
                                                ? builder_.CreateZExt(lhs, rhs->getType(), "cmp.lzx")
                                                : builder_.CreateSExt(lhs, rhs->getType(), "cmp.lsx");
                                        } else {
                                            rhs = isUnsignedInt
                                                ? builder_.CreateZExt(rhs, lhs->getType(), "cmp.rzx")
                                                : builder_.CreateSExt(rhs, lhs->getType(), "cmp.rsx");
                                        }
                                    }
                                }
                                // Emit: (a > b) as i32 - (a < b) as i32
                                llvm::Value* gt, *lt;
                                if (isFP) {
                                    gt = builder_.CreateFCmpOGT(lhs, rhs, "cmp.gt");
                                    lt = builder_.CreateFCmpOLT(lhs, rhs, "cmp.lt");
                                } else if (isUnsignedInt) {
                                    gt = builder_.CreateICmpUGT(lhs, rhs, "cmp.gt");
                                    lt = builder_.CreateICmpULT(lhs, rhs, "cmp.lt");
                                } else {
                                    gt = builder_.CreateICmpSGT(lhs, rhs, "cmp.gt");
                                    lt = builder_.CreateICmpSLT(lhs, rhs, "cmp.lt");
                                }
                                llvm::Value* gtI = builder_.CreateZExt(gt, i32(), "cmp.gti");
                                llvm::Value* ltI = builder_.CreateZExt(lt, i32(), "cmp.lti");
                                return builder_.CreateSub(gtI, ltI, "cmp.res");
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
                            // stdlib §9.1 (#518): ToString(radix: int) — the arg is an
                            // integer (not a string format literal). Route to the same
                            // inline radix-conversion logic as Convert.ToString(n, radix).
                            // Supported radices: 2, 8, 10, 16 (per stdlib §9.1).
                            {
                                using P = sema::PrimitiveType;
                                bool isIntRecv = (tt->primitive == P::Int ||
                                                  tt->primitive == P::Long ||
                                                  tt->primitive == P::UInt ||
                                                  tt->primitive == P::ULong ||
                                                  tt->primitive == P::Byte ||
                                                  tt->primitive == P::SByte ||
                                                  tt->primitive == P::Short ||
                                                  tt->primitive == P::UShort);
                                auto* argTy = ctx_.exprType(*call->args[0].value);
                                bool argIsInt = argTy &&
                                    argTy->kind == sema::TypeRef::Kind::Primitive &&
                                    (argTy->primitive == P::Int ||
                                     argTy->primitive == P::Long);
                                if (isIntRecv && argIsInt) {
                                    llvm::Value* radix = lowerExpr(*call->args[0].value);
                                    if (!radix) return nullptr;
                                    // Normalize receiver to i32 (or i64 for long).
                                    bool isLong = (tt->primitive == P::Long ||
                                                   tt->primitive == P::ULong);
                                    if (isLong) {
                                        if (!recv->getType()->isIntegerTy(64))
                                            recv = builder_.CreateSExt(recv, i64());
                                    } else {
                                        if (!recv->getType()->isIntegerTy(32))
                                            recv = builder_.CreateSExt(recv, i32());
                                    }
                                    // Resolve radix from a constant int.
                                    const char* fmt = "%d";  // fallback: decimal
                                    bool isBinary = false;
                                    if (auto* cr = llvm::dyn_cast<llvm::ConstantInt>(radix)) {
                                        uint64_t r = cr->getZExtValue();
                                        if (r == 16) fmt = isLong ? "%llx" : "%x";
                                        else if (r == 8) fmt = isLong ? "%llo" : "%o";
                                        else if (r == 2) isBinary = true;
                                        else fmt = isLong ? "%lld" : "%d";
                                    } else {
                                        // Non-constant radix: fall back to decimal.
                                        fmt = isLong ? "%lld" : "%d";
                                    }
                                    // Binary path: emit a bit-scan loop.
                                    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                                    const unsigned BUF = 66;  // 64 bits + sign + NUL
                                    if (isBinary) {
                                        // Reuse the Convert.ToString(n,2) inline logic.
                                        // Widen to i64 so the same loop handles both int and long.
                                        llvm::Value* n64 = isLong
                                            ? recv
                                            : builder_.CreateSExt(recv, i64());
                                        auto* buf2 = entryAlloca(llvm::ArrayType::get(i8Ty, BUF));
                                        llvm::Value* b2Ptr = builder_.CreateGEP(
                                            llvm::ArrayType::get(i8Ty, BUF), buf2,
                                            {builder_.getInt64(0), builder_.getInt64(0)});
                                        llvm::Value* isZero = builder_.CreateICmpEQ(
                                            n64, builder_.getInt64(0));
                                        auto* zeroBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.z", currentFn_);
                                        auto* nonBB  = llvm::BasicBlock::Create(
                                            llvm_, "bn2.nz", currentFn_);
                                        auto* binBB  = llvm::BasicBlock::Create(
                                            llvm_, "bn2.e", currentFn_);
                                        builder_.CreateCondBr(isZero, zeroBB, nonBB);
                                        // zero → "0"
                                        builder_.SetInsertPoint(zeroBB);
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, '0'), b2Ptr);
                                        llvm::Value* z1 = builder_.CreateGEP(
                                            i8Ty, b2Ptr, builder_.getInt64(1));
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, 0), z1);
                                        llvm::Value* zlen = builder_.getInt64(1);
                                        auto* zeroBBEnd = builder_.GetInsertBlock();
                                        builder_.CreateBr(binBB);
                                        // non-zero: scan MSB then emit each bit.
                                        builder_.SetInsertPoint(nonBB);
                                        auto* bitPos = entryAlloca(i32());
                                        builder_.CreateStore(builder_.getInt32(63), bitPos);
                                        auto* fhBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.fh", currentFn_);
                                        auto* fhBodyBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.fhb", currentFn_);
                                        auto* fhEndBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.fhe", currentFn_);
                                        builder_.CreateBr(fhBB);
                                        builder_.SetInsertPoint(fhBB);
                                        llvm::Value* bp = builder_.CreateLoad(i32(), bitPos);
                                        llvm::Value* bpGe0 = builder_.CreateICmpSGT(
                                            bp, builder_.getInt32(0));
                                        llvm::Value* bpW = builder_.CreateZExt(bp, i64());
                                        llvm::Value* bit = builder_.CreateAnd(
                                            builder_.CreateLShr(n64, bpW),
                                            builder_.getInt64(1));
                                        llvm::Value* isOne = builder_.CreateICmpEQ(
                                            bit, builder_.getInt64(1));
                                        builder_.CreateCondBr(
                                            builder_.CreateAnd(bpGe0,
                                                builder_.CreateNot(isOne)),
                                            fhBodyBB, fhEndBB);
                                        builder_.SetInsertPoint(fhBodyBB);
                                        builder_.CreateStore(
                                            builder_.CreateSub(bp, builder_.getInt32(1)),
                                            bitPos);
                                        builder_.CreateBr(fhBB);
                                        builder_.SetInsertPoint(fhEndBB);
                                        auto* emBP  = entryAlloca(i32());
                                        auto* emLen = entryAlloca(i64());
                                        llvm::Value* startBP = builder_.CreateLoad(
                                            i32(), bitPos);
                                        builder_.CreateStore(startBP, emBP);
                                        builder_.CreateStore(builder_.getInt64(0), emLen);
                                        auto* emCBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.ec", currentFn_);
                                        auto* emBBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.eb", currentFn_);
                                        auto* emEBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.ee", currentFn_);
                                        builder_.CreateBr(emCBB);
                                        builder_.SetInsertPoint(emCBB);
                                        llvm::Value* ep = builder_.CreateLoad(i32(), emBP);
                                        builder_.CreateCondBr(
                                            builder_.CreateICmpSGE(ep, builder_.getInt32(0)),
                                            emBBB, emEBB);
                                        builder_.SetInsertPoint(emBBB);
                                        llvm::Value* epB = builder_.CreateLoad(i32(), emBP);
                                        llvm::Value* lenV = builder_.CreateLoad(i64(), emLen);
                                        llvm::Value* epW = builder_.CreateZExt(epB, i64());
                                        llvm::Value* d = builder_.CreateAnd(
                                            builder_.CreateLShr(n64, epW),
                                            builder_.getInt64(1));
                                        llvm::Value* ch = builder_.CreateAdd(
                                            builder_.CreateTrunc(d, i8Ty),
                                            llvm::ConstantInt::get(i8Ty, '0'));
                                        llvm::Value* dp = builder_.CreateGEP(
                                            i8Ty, b2Ptr, lenV);
                                        builder_.CreateStore(ch, dp);
                                        builder_.CreateStore(
                                            builder_.CreateAdd(lenV, builder_.getInt64(1)),
                                            emLen);
                                        builder_.CreateStore(
                                            builder_.CreateSub(epB, builder_.getInt32(1)),
                                            emBP);
                                        builder_.CreateBr(emCBB);
                                        builder_.SetInsertPoint(emEBB);
                                        llvm::Value* nlen = builder_.CreateLoad(i64(), emLen);
                                        llvm::Value* np2 = builder_.CreateGEP(
                                            i8Ty, b2Ptr, nlen);
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, 0), np2);
                                        auto* nonBBEnd = builder_.GetInsertBlock();
                                        builder_.CreateBr(binBB);
                                        // merge
                                        builder_.SetInsertPoint(binBB);
                                        auto* phiLen = builder_.CreatePHI(i64(), 2, "bn2.len");
                                        phiLen->addIncoming(zlen, zeroBBEnd);
                                        phiLen->addIncoming(nlen, nonBBEnd);
                                        auto* mallocFn2 = declareC("malloc", ptrTy(), {i64()});
                                        auto* memcpyFn2 = declareC("memcpy", ptrTy(),
                                                                    {ptrTy(), ptrTy(), i64()});
                                        llvm::Value* dSz2 = builder_.CreateAdd(
                                            phiLen, builder_.getInt64(1));
                                        llvm::Value* dc2 = builder_.CreateCall(
                                            mallocFn2, {dSz2}, "bn2.d");
                                        builder_.CreateCall(memcpyFn2, {dc2, b2Ptr, phiLen});
                                        llvm::Value* np3 = builder_.CreateGEP(
                                            i8Ty, dc2, phiLen);
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, 0), np3);
                                        uint64_t sBytes2 = module_->getDataLayout()
                                            .getTypeAllocSize(stringType());
                                        llvm::Value* so2 = builder_.CreateCall(
                                            mallocFn2,
                                            {builder_.getInt64(sBytes2)}, "bn2.o");
                                        builder_.CreateStore(
                                            dc2,
                                            builder_.CreateStructGEP(stringType(), so2, 0));
                                        builder_.CreateStore(
                                            phiLen,
                                            builder_.CreateStructGEP(stringType(), so2, 1));
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, 1),
                                            builder_.CreateStructGEP(stringType(), so2, 2));
                                        initStringRc(so2);
                                        return so2;
                                    }
                                    // Non-binary: snprintf with the chosen format.
                                    llvm::Constant* fmtData2 = llvm::ConstantDataArray::getString(
                                        llvm_, fmt, true);
                                    unsigned fid2 = stringLiteralIdx_++;
                                    auto* fmtGV2 = new llvm::GlobalVariable(
                                        *module_, fmtData2->getType(), true,
                                        llvm::GlobalValue::PrivateLinkage, fmtData2,
                                        ".bz.rts.fmt." + std::to_string(fid2));
                                    fmtGV2->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                                    auto* snpFn2 = module_->getFunction("snprintf");
                                    if (!snpFn2) {
                                        auto* ft2 = llvm::FunctionType::get(
                                            i32(), {ptrTy(), i64(), ptrTy()}, true);
                                        snpFn2 = llvm::Function::Create(
                                            ft2, llvm::Function::ExternalLinkage,
                                            "snprintf", module_.get());
                                    }
                                    auto* buf3 = entryAlloca(llvm::ArrayType::get(i8Ty, BUF));
                                    llvm::Value* bPtr3 = builder_.CreateGEP(
                                        llvm::ArrayType::get(i8Ty, BUF), buf3,
                                        {builder_.getInt64(0), builder_.getInt64(0)});
                                    llvm::Value* wr2 = builder_.CreateCall(
                                        snpFn2,
                                        {bPtr3, builder_.getInt64(BUF), fmtGV2, recv},
                                        "rts.wr");
                                    auto* mallocFn3 = declareC("malloc", ptrTy(), {i64()});
                                    auto* memcpyFn3 = declareC("memcpy", ptrTy(),
                                                               {ptrTy(), ptrTy(), i64()});
                                    llvm::Value* l642 = builder_.CreateSExt(wr2, i64());
                                    llvm::Value* dSz3 = builder_.CreateAdd(
                                        l642, builder_.getInt64(1));
                                    llvm::Value* dc3 = builder_.CreateCall(
                                        mallocFn3, {dSz3}, "rts.d");
                                    builder_.CreateCall(memcpyFn3, {dc3, bPtr3, l642});
                                    llvm::Value* np4 = builder_.CreateGEP(
                                        i8Ty, dc3, l642);
                                    builder_.CreateStore(
                                        llvm::ConstantInt::get(i8Ty, 0), np4);
                                    uint64_t sBytes3 = module_->getDataLayout()
                                        .getTypeAllocSize(stringType());
                                    llvm::Value* so3 = builder_.CreateCall(
                                        mallocFn3,
                                        {builder_.getInt64(sBytes3)}, "rts.o");
                                    builder_.CreateStore(
                                        dc3,
                                        builder_.CreateStructGEP(stringType(), so3, 0));
                                    builder_.CreateStore(
                                        l642,
                                        builder_.CreateStructGEP(stringType(), so3, 1));
                                    builder_.CreateStore(
                                        llvm::ConstantInt::get(i8Ty, 1),
                                        builder_.CreateStructGEP(stringType(), so3, 2));
                                    initStringRc(so3);
                                    return so3;
                                }
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
                // §3.4 / issue #474: a Range index `s[a..b]` returns the
                // SUBSTRING spanning the range — a fresh `string` — rather than
                // a single `char`. Intercept the range form before the
                // code-point path (the index is a RangeExpr, not a value).
                if (auto* range = dynamic_cast<const ast::RangeExpr*>(
                        ix->indices[0].value.get())) {
                    llvm::Value* strPtr = lowerExpr(*ix->target);
                    if (!strPtr) return nullptr;
                    return emitStringRangeSlice(strPtr, *range);
                }
                // §4.6: a single int index `s[i]` returns the i-th char.
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
                // issue #462 / §7.5 / §14.1: for a GENERIC nullable record
                // (`Box<int>?` / `Box<string>?`) install the receiver's concrete
                // type args so fieldIndex_ is registered against — and the field
                // type resolves under — the SPECIALIZED record struct (the same
                // layout the nullable wrapper's payload uses). recordStruct(ms's
                // parent) re-registers fieldIndex_ for this instantiation; without
                // it the placeholder layout's index/type is used. A no-op for a
                // non-generic nullable record.
                RecordSubstGuard ncSubst(this, tt);
                recordStruct(ms->parent);  // register specialized fieldIndex_
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
                // #462: resolve a generic field's `T` to its concrete type so the
                // nullable-value detection below classifies it correctly.
                const sema::TypeRef* fieldTy = recordMemberTypeResolved(ms);
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
            // per §6.8 / #652: Domain instance property reads.
            if (cn == "Domain" && ma->member == "Name") {
                llvm::Value* dom = lowerExpr(*ma->target);
                if (!dom) return nullptr;
                auto* fn = declareC("bz_domain_get_name", ptrTy(), {ptrTy()});
                return builder_.CreateCall(fn, {dom}, "dom.name");
            }
            // stdlib §5 (#651): IGrouping<TKey, T>.Key property read.
            // IGrouping objects are bz_grouping_t* pointers; the key is
            // stored as key_esz bytes at group->key_data. bz_grouping_key
            // copies the key bytes into a caller-allocated slot.
            // The key type (TKey) determines the LLVM type for the slot.
            if (cn == "IGrouping" && ma->member == "Key" && tt &&
                !tt->genericArgs.empty() && tt->genericArgs[0]) {
                llvm::Value* grp = lowerExpr(*ma->target);
                if (!grp) return nullptr;
                sema::TypeRef* keyRef = tt->genericArgs[0];
                llvm::Type* keyTy = lowerTypeRef(keyRef);
                if (!keyTy) keyTy = i32();
                llvm::AllocaInst* keySlot = entryAlloca(keyTy);
                auto* fn = declareC("bz_grouping_key", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {grp, keySlot});
                return builder_.CreateLoad(keyTy, keySlot, "grp.key");
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
                // stdlib §9.3 / fix #521: Decimal static constants.
                if (id2->name == "Decimal") {
                    llvm::StructType* dst = decimalStructType();
                    const auto& mbr = ma->member;
                    if (mbr == "Zero")
                        return llvm::ConstantAggregateZero::get(dst);
                    if (mbr == "One") {
                        uint32_t dec[4] = {0, 0, 0, 0};
                        bz_decimal_parse_cstr("1", dec);
                        return llvm::ConstantStruct::get(dst, {
                            llvm::ConstantInt::get(i32(), dec[0]),
                            llvm::ConstantInt::get(i32(), dec[1]),
                            llvm::ConstantInt::get(i32(), dec[2]),
                            llvm::ConstantInt::get(i32(), dec[3])
                        });
                    }
                    if (mbr == "MinValue") {
                        uint32_t dec[4] = {0, 0, 0, 0};
                        bz_decimal_parse_cstr("-79228162514264337593543950335", dec);
                        return llvm::ConstantStruct::get(dst, {
                            llvm::ConstantInt::get(i32(), dec[0]),
                            llvm::ConstantInt::get(i32(), dec[1]),
                            llvm::ConstantInt::get(i32(), dec[2]),
                            llvm::ConstantInt::get(i32(), dec[3])
                        });
                    }
                    if (mbr == "MaxValue") {
                        uint32_t dec[4] = {0, 0, 0, 0};
                        bz_decimal_parse_cstr("79228162514264337593543950335", dec);
                        return llvm::ConstantStruct::get(dst, {
                            llvm::ConstantInt::get(i32(), dec[0]),
                            llvm::ConstantInt::get(i32(), dec[1]),
                            llvm::ConstantInt::get(i32(), dec[2]),
                            llvm::ConstantInt::get(i32(), dec[3])
                        });
                    }
                }
            }
        }
    }
    // fix #499 / §9.8: Range<T> instance member access.
    // The Range struct is { i64 start, i64 end, i64 step, i8 flags }.
    // flags bit 0 = IsEndInclusive; bit 1 = startOpen; bit 2 = endOpen.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            if (isRangeType(tt)) {
                llvm::Value* rv = lowerExpr(*ma->target);
                if (!rv) return nullptr;
                const auto& mbr = ma->member;
                if (mbr == "Start") {
                    // §9.8: Start is T? (nullable). For the value-in-var case we
                    // return the i64 lo field directly, cast to i32 (int range).
                    // TODO: open-ended ranges (bit 1 of flags) should return null.
                    llvm::Value* lo = builder_.CreateExtractValue(rv, {0u}, "r.start");
                    // Narrow to i32 for int-range Start (T = int → i32).
                    return builder_.CreateTrunc(lo, i32(), "r.start.i32");
                }
                if (mbr == "End") {
                    llvm::Value* hi = builder_.CreateExtractValue(rv, {1u}, "r.end");
                    return builder_.CreateTrunc(hi, i32(), "r.end.i32");
                }
                if (mbr == "Step") {
                    // §9.8: Step is long (i64).
                    return builder_.CreateExtractValue(rv, {2u}, "r.step");
                }
                if (mbr == "IsEndInclusive") {
                    // flags bit 0.
                    llvm::Value* flg = builder_.CreateExtractValue(rv, {3u}, "r.flg");
                    return builder_.CreateTrunc(flg, i1(), "r.incl");
                }
                if (mbr == "IsOpenEnded") {
                    // §9.8: true if Start or End is null (bits 1 or 2 of flags).
                    llvm::Value* flg = builder_.CreateExtractValue(rv, {3u}, "r.flg");
                    // (flags >> 1) & 3 != 0  →  startOpen or endOpen set.
                    llvm::Value* shifted = builder_.CreateLShr(
                        flg, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1));
                    llvm::Value* masked  = builder_.CreateAnd(
                        shifted,
                        llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 3));
                    return builder_.CreateICmpNE(
                        masked,
                        llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0),
                        "r.open");
                }
                if (mbr == "IsEmpty") {
                    // §9.8: a range is empty when it contains no values.
                    // For an ascending range with Step=1 (default):
                    //   inclusive (..)  : empty if hi < lo
                    //   half-open (..<) : empty if hi <= lo
                    // For Step != 1 (not yet fully general), we approximate with
                    // the same endpoint test (correct for default-step ranges).
                    llvm::Value* lo  = builder_.CreateExtractValue(rv, {0u}, "r.lo");
                    llvm::Value* hi  = builder_.CreateExtractValue(rv, {1u}, "r.hi");
                    llvm::Value* flg = builder_.CreateExtractValue(rv, {3u}, "r.flg");
                    llvm::Value* incl = builder_.CreateTrunc(flg, i1(), "r.incl");
                    // inclusive: empty when hi < lo (i.e. NOT hi >= lo).
                    llvm::Value* emptyIncl = builder_.CreateICmpSLT(hi, lo, "r.ei");
                    // half-open: empty when hi <= lo (i.e. NOT hi > lo).
                    llvm::Value* emptyHalf = builder_.CreateICmpSLE(hi, lo, "r.eh");
                    return builder_.CreateSelect(incl, emptyIncl, emptyHalf, "r.empty");
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
    // §4.15 (#549): ArraySpan<T>/ReadOnlyArraySpan<T> readonly property reads.
    // Both types are value structs { ptr source, i32 start, i32 length }.
    // Length extracts field 2; IsEmpty compares field 2 to 0.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target &&
            (ma->member == "Length" || ma->member == "IsEmpty")) {
            auto* tt = ctx_.exprType(*ma->target);
            if (isArraySpanTypeName(tt)) {
                llvm::Value* sv = lowerExpr(*ma->target);
                if (!sv) return nullptr;
                llvm::Value* slen = builder_.CreateExtractValue(
                    sv, {2u}, "sp.len");
                if (ma->member == "Length")
                    return slen;
                // IsEmpty: length == 0
                return builder_.CreateICmpEQ(
                    slen, builder_.getInt32(0), "sp.empty");
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
                    // §28.2 v0.135 / issue #627: checked-disposal guard for
                    // array-backed collection Count/Length property access.
                    // Skip for Set<T> (dict-backed); they use bz_dict_count.
                    if (checkedDisposal_ && !dictBacked) {
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
                        auto* isFn = declareC("bz_array_is_disposed", i8Ty,
                                              {ptrTy()});
                        llvm::Value* flag =
                            builder_.CreateCall(isFn, {obj}, "cd.arrdis");
                        llvm::Value* disposed = builder_.CreateICmpNE(
                            flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
                        emitGuardThrow(disposed, "DisposedAccessException");
                    }
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
                    // §28.2 v0.135 / issue #627: checked-disposal guard for
                    // dict Count property access.
                    if (checkedDisposal_) {
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
                        auto* isFn = declareC("bz_dict_is_disposed", i8Ty,
                                              {ptrTy()});
                        llvm::Value* flag =
                            builder_.CreateCall(isFn, {obj}, "cd.ddis");
                        llvm::Value* disposed = builder_.CreateICmpNE(
                            flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
                        emitGuardThrow(disposed, "DisposedAccessException");
                    }
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
                // per §4.6 (stdlib L1264–1265): `First`/`Last` are typed `T?`.
                // The getter yields `null` when the list is empty (the node
                // pointer is null) and the element value otherwise. This null
                // representation is required for §7.11 lifting and `??` to
                // interoperate with the result — every consumer reads `l.First`
                // as the nullable form sema assigned it.
                {
                    const char* fn = ma->member == "First"
                        ? "bz_llist_first_node" : "bz_llist_last_node";
                    auto* nodeF = declareC(fn, ptrTy(), {ptrTy()});
                    llvm::Value* nodePtr = builder_.CreateCall(nodeF, {list}, "ll.fnode");
                    llvm::Value* nodeNull = builder_.CreateICmpEQ(
                        nodePtr, llvm::ConstantPointerNull::get(ptrTy()),
                        "ll.empty");
                    const sema::TypeRef* resTy = ctx_.exprType(e);
                    // For a NULLABLE VALUE element type (int?, double?, …) the
                    // result is the tagged {i8 hasValue; T value} struct, built
                    // only on the present branch so the value load never runs on
                    // an empty list (a null `valPtr`).
                    if (resTy && isNullableValueTy(resTy)) {
                        auto* entryBB = builder_.GetInsertBlock();
                        auto* presentBB =
                            llvm::BasicBlock::Create(llvm_, "ll.first.val", currentFn_);
                        auto* mergeBB =
                            llvm::BasicBlock::Create(llvm_, "ll.first.end", currentFn_);
                        auto* st = nullableValueStructTy(resTy);
                        builder_.CreateCondBr(nodeNull, mergeBB, presentBB);
                        builder_.SetInsertPoint(presentBB);
                        auto* valF = declareC("bz_llist_node_value_ptr",
                                              ptrTy(), {ptrTy()});
                        llvm::Value* valPtr =
                            builder_.CreateCall(valF, {nodePtr}, "ll.vptr");
                        llvm::Value* loaded =
                            builder_.CreateLoad(elemLLTy, valPtr, "ll.val");
                        llvm::Value* wrapped = wrapNullableValue(resTy, loaded);
                        auto* presentEndBB = builder_.GetInsertBlock();
                        builder_.CreateBr(mergeBB);
                        builder_.SetInsertPoint(mergeBB);
                        auto* phi = builder_.CreatePHI(st, 2, "ll.first");
                        phi->addIncoming(nullableValueNull(resTy), entryBB);
                        phi->addIncoming(wrapped, presentEndBB);
                        return phi;
                    }
                    // For a reference (class/string/interface) element type, T?
                    // is a nullable pointer: the node-value slot holds the
                    // pointer; an empty list yields a null pointer directly.
                    auto* valF = declareC("bz_llist_node_value_ptr",
                                          ptrTy(), {ptrTy()});
                    llvm::Value* valPtr =
                        builder_.CreateCall(valF, {nodePtr}, "ll.vptr");
                    if (elemLLTy->isPointerTy()) {
                        // Read the slot only when the node is present; an empty
                        // list returns a null reference.
                        auto* entryBB = builder_.GetInsertBlock();
                        auto* presentBB =
                            llvm::BasicBlock::Create(llvm_, "ll.first.ref", currentFn_);
                        auto* mergeBB =
                            llvm::BasicBlock::Create(llvm_, "ll.first.refend", currentFn_);
                        builder_.CreateCondBr(nodeNull, mergeBB, presentBB);
                        builder_.SetInsertPoint(presentBB);
                        llvm::Value* loaded =
                            builder_.CreateLoad(elemLLTy, valPtr, "ll.refval");
                        auto* presentEndBB = builder_.GetInsertBlock();
                        builder_.CreateBr(mergeBB);
                        builder_.SetInsertPoint(mergeBB);
                        auto* phi = builder_.CreatePHI(elemLLTy, 2, "ll.first.ref");
                        phi->addIncoming(
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(elemLLTy)),
                            entryBB);
                        phi->addIncoming(loaded, presentEndBB);
                        return phi;
                    }
                    // Fallback (no resolved nullable result type): load the raw
                    // element value as before.
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
    // fix #571 / §4.8: Dictionary<K,V>.Keys — property returning Array<K> of
    // all keys.  Previously this always called bz_dict_keys_str, which reads the
    // string-key table and returns garbage/crash for non-string (generic-table)
    // keys (int, enum, record).  Now dispatches like lowerDictKeysFor (Stmts.cpp):
    // generic-table keys (K != string) → bz_dict_keys_gen path; string keys →
    // existing bz_dict_keys_str path.  The for-in handler (lowerDictKeysFor) is
    // the correctness reference for both paths.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Keys") {
            auto* tt = ctx_.exprType(*ma->target);
            if (isDictTypeName(tt)) {
                llvm::Value* dictObj = lowerExpr(*ma->target);
                if (!dictObj) return nullptr;

                // Determine key type and whether it routes through the generic table.
                const sema::TypeRef* genKeyTy =
                    genericBackingType(tt, 0, /*isSet=*/false, nullptr);

                if (genKeyTy) {
                    // fix #571 / §4.8: non-string key — use bz_dict_keys_gen.
                    // bz_dict_keys_gen returns a malloc'd void** array of `n`
                    // pointers, each a malloc'd copy of one key's raw bytes.
                    // Load each key with genKeyTy's LLVM type, store into the
                    // result Array<K> — mirrors emitGenericKeyTableFor (Stmts.cpp)
                    // and the Values expression handler above.
                    llvm::Type* keyLlvm = lowerTypeRef(genKeyTy);
                    if (!keyLlvm) return nullptr;
                    int64_t esz = collectionElemSize(ctx_.exprType(e));

                    auto* keysOutSlot = entryAlloca(ptrTy());
                    auto* cntSlot     = entryAlloca(i64());
                    auto* keysFn = declareC("bz_dict_keys_gen",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy(), ptrTy(), ptrTy()});
                    builder_.CreateCall(keysFn, {dictObj, keysOutSlot, cntSlot});
                    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);
                    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);
                    auto* freeFn = declareC("free",
                                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                    auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                    llvm::Value* result =
                        builder_.CreateCall(allocFn,
                                            {builder_.getInt64(esz), cnt}, "dk.arr");
                    auto* addFn = declareC("bz_array_add",
                                           llvm::Type::getVoidTy(llvm_),
                                           {ptrTy(), ptrTy(), i64()});

                    auto* idxSlot = entryAlloca(i64());
                    builder_.CreateStore(builder_.getInt64(0), idxSlot);
                    auto* condBB = llvm::BasicBlock::Create(llvm_, "dk2.c", currentFn_);
                    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dk2.b", currentFn_);
                    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dk2.e", currentFn_);
                    builder_.CreateBr(condBB);

                    builder_.SetInsertPoint(condBB);
                    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
                    builder_.CreateCondBr(
                        builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

                    builder_.SetInsertPoint(bodyBB);
                    llvm::Value* idxB    = builder_.CreateLoad(i64(), idxSlot);
                    llvm::Value* kpPtr   = builder_.CreateGEP(ptrTy(), keysArr, idxB);
                    llvm::Value* keyPtr  = builder_.CreateLoad(ptrTy(), kpPtr);
                    // Load the key value from its raw-bytes copy (mirrors
                    // emitGenericKeyTableFor: CreateLoad(elemTy, keyPtr)).
                    llvm::Value* keyVal  = builder_.CreateLoad(keyLlvm, keyPtr,
                                                               "dk2.kv");
                    llvm::AllocaInst* keySlot = entryAlloca(keyLlvm);
                    builder_.CreateStore(keyVal, keySlot);
                    builder_.CreateCall(addFn,
                                        {result, keySlot, builder_.getInt64(esz)});
                    // Free this iteration's key-bytes copy (mirrors lowerDictKeysFor
                    // and emitGenericKeyTableFor).
                    builder_.CreateCall(freeFn, {keyPtr});
                    builder_.CreateStore(
                        builder_.CreateAdd(idxB, builder_.getInt64(1)), idxSlot);
                    builder_.CreateBr(condBB);

                    builder_.SetInsertPoint(endBB);
                    builder_.CreateCall(freeFn, {keysArr});
                    return result;
                }

                // String keys: existing bz_dict_keys_str path (unchanged).
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
    // fix #556 / fix #563 / §4.8: `Dictionary<K,V>.Values` as an expression —
    // returns a fresh bz_array_t* of decoded values.  Mirrors the Keys handler
    // above but uses genericArgs[1] (the value type) and the bz_dict_values_*
    // snapshots.  The element decoding follows the same void*-encoding rules as
    // lowerDictValuesFor (LlvmBackend_Stmts.cpp): record → load the struct from
    // the heap box (fix #563); nullable value type → unbox tagged struct;
    // scalar → ptrtoint + trunc; pointer (string / class) → void* IS the value.
    // fix #563: the old code stored the raw box pointer (ptrTy()) into elemSlot
    // and called bz_array_add with the struct's byte-count; bz_array_add then
    // copied struct-size bytes out of an 8-byte pointer slot → garbage.  The
    // correct decode mirrors lowerCollectionIndexerGet §4.8: load the struct
    // bytes out of the box (CreateLoad(valLlvm, slotV)) and put the struct value
    // into a properly-typed elemSlot so bz_array_add copies the real bytes.
    // The resulting bz_array_t* is typed as Array<V> by sema (fix #556), so
    // collectionElemSize will compute the correct per-element stride and
    // downstream .Count / .ToArray() / LINQ will work correctly.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Values") {
            auto* dictTy = ctx_.exprType(*ma->target);
            if (isDictTypeName(dictTy) &&
                dictTy->genericArgs.size() >= 2 &&
                dictTy->genericArgs[1]) {
                const sema::TypeRef* valTy = dictTy->genericArgs[1];
                llvm::Type* valLlvm = lowerTypeRef(valTy);
                if (valLlvm) {
                    // Determine value category (matches lowerDictValuesFor).
                    const sema::TypeSymbol* valRec =
                        (valTy->kind == sema::TypeRef::Kind::Named &&
                         valTy->declaration &&
                         valTy->declaration->kind == ast::TypeDeclKind::Record)
                            ? valTy->declaration : nullptr;
                    bool valNullableVal = isNullableValueTy(valTy);

                    llvm::Value* dictObj = lowerExpr(*ma->target);
                    if (!dictObj) return nullptr;

                    // Snapshot the values array via the string-key or generic-key
                    // helper (same dispatch as lowerDictValuesFor).
                    auto* valsOutSlot = entryAlloca(ptrTy());
                    auto* cntSlot     = entryAlloca(i64());
                    bool useGen =
                        genericBackingType(dictTy, 0, /*isSet=*/false, nullptr)
                        != nullptr;
                    if (useGen) {
                        auto* fn = declareC("bz_dict_values_gen",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy(), ptrTy(), ptrTy()});
                        builder_.CreateCall(fn, {dictObj, valsOutSlot, cntSlot});
                    } else {
                        auto* fn = declareC("bz_dict_values_str",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy(), ptrTy(), ptrTy()});
                        builder_.CreateCall(fn, {dictObj, valsOutSlot, cntSlot});
                    }
                    llvm::Value* valsArr =
                        builder_.CreateLoad(ptrTy(), valsOutSlot);
                    llvm::Value* cnt = builder_.CreateLoad(i64(), cntSlot);

                    // Allocate the result array with the correct element stride.
                    int64_t esz = collectionElemSize(
                        ctx_.exprType(e));  // exprType(e) is the Array<V> TypeRef
                    auto* allocFn = declareC("bz_array_alloc", ptrTy(),
                                             {i64(), i64()});
                    llvm::Value* result =
                        builder_.CreateCall(allocFn,
                                            {builder_.getInt64(esz), cnt},
                                            "dv.arr");
                    auto* addFn = declareC("bz_array_add",
                                           llvm::Type::getVoidTy(llvm_),
                                           {ptrTy(), ptrTy(), i64()});
                    auto* freeFn = declareC("free",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy()});

                    // Loop over the snapshot, decode each void* → V, add to result.
                    auto* idxSlot = entryAlloca(i64());
                    builder_.CreateStore(builder_.getInt64(0), idxSlot);
                    auto* condBB = llvm::BasicBlock::Create(llvm_, "dve.c",
                                                            currentFn_);
                    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dve.b",
                                                            currentFn_);
                    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dve.e",
                                                            currentFn_);
                    builder_.CreateBr(condBB);

                    builder_.SetInsertPoint(condBB);
                    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
                    builder_.CreateCondBr(
                        builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

                    builder_.SetInsertPoint(bodyBB);
                    llvm::Value* idxB  = builder_.CreateLoad(i64(), idxSlot);
                    llvm::Value* vpPtr = builder_.CreateGEP(ptrTy(), valsArr,
                                                            idxB);
                    llvm::Value* slotV = builder_.CreateLoad(ptrTy(), vpPtr);

                    // Decode void* → V (same encoding as lowerDictValuesFor,
                    // fixed by #563 to match lowerCollectionIndexerGet §4.8).
                    llvm::Value* decoded;
                    if (valRec) {
                        // fix #563 / §4.8: load the record struct from the
                        // heap box.  The runtime boxes records (malloc+memcpy)
                        // so they fit in the void* slot; decode by loading the
                        // struct type back out of the box pointer — the same
                        // path the direct dict indexer get uses.
                        decoded = builder_.CreateLoad(valLlvm, slotV, "dve.rec");
                    } else if (valNullableVal && valLlvm->isStructTy()) {
                        decoded = builder_.CreateLoad(valLlvm, slotV,
                                                      "dve.nvval");
                    } else if (!valLlvm->isPointerTy()) {
                        llvm::Value* asI64 =
                            builder_.CreatePtrToInt(slotV, i64());
                        if (valLlvm->isDoubleTy())
                            decoded = builder_.CreateBitCast(asI64, valLlvm);
                        else if (valLlvm->isFloatTy())
                            decoded = builder_.CreateBitCast(
                                builder_.CreateTrunc(asI64, i32()), valLlvm);
                        else if (valLlvm->isIntegerTy(64))
                            decoded = asI64;
                        else
                            decoded = builder_.CreateTrunc(asI64, valLlvm);
                    } else {
                        decoded = slotV;  // pointer (string / class object)
                    }

                    // Store the decoded element into a stack slot and add to
                    // array.  fix #563: elemSlot must be typed as decoded's
                    // type (always decoded->getType() now that records produce
                    // a struct value from the load above, not a ptrTy()).
                    // bz_array_add copies esz bytes from elemSlot; the slot
                    // must hold the actual struct bytes, not a pointer to them.
                    llvm::AllocaInst* elemSlot = entryAlloca(decoded->getType());
                    builder_.CreateStore(decoded, elemSlot);
                    builder_.CreateCall(addFn,
                                        {result, elemSlot,
                                         builder_.getInt64(esz)});

                    builder_.CreateStore(
                        builder_.CreateAdd(idxB, builder_.getInt64(1)),
                        idxSlot);
                    builder_.CreateBr(condBB);

                    builder_.SetInsertPoint(endBB);
                    builder_.CreateCall(freeFn, {valsArr});
                    return result;
                }
            }
        }
    }
    // CG22+: Exception.Message — built-in exceptions store a message string
    // pointer at byte offset 8 (struct slot 1) of the exception object.
    //
    // #595: user-defined classes that directly inherit a built-in exception
    // (e.g. `class ErrA: Exception`) embed the same message-ptr at struct
    // slot 1 (byte offset 8) as part of the inherited layout inserted by
    // classStruct.  Both cases use the same GEP-at-byte-8 path; the
    // recognition is extended to cover the user-class case via
    // hasBuiltinExceptionBase.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Message") {
            auto* tt = ctx_.exprType(*ma->target);
            // Case A: receiver is a built-in exception TypeRef (no user declaration).
            bool isBuiltinEx = tt && isBuiltinExceptionType(tt);
            // Case B: receiver is a user class that directly inherits a built-in
            // exception — it embeds the message slot at the same byte offset 8.
            bool isUserDerivedEx = false;
            if (!isBuiltinEx && tt &&
                tt->kind == sema::TypeRef::Kind::Named && tt->declaration)
                isUserDerivedEx = hasBuiltinExceptionBase(tt->declaration);
            if (isBuiltinEx || isUserDerivedEx) {
                llvm::Value* excObj = lowerExpr(*ma->target);
                if (!excObj) return nullptr;
                // The message ptr is always at byte offset 8 — slot 1 of the
                // raw built-in layout AND slot 1 of the user-class struct
                // (classStruct inserts it before the disposed flag).
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
    // issue #494 / §29.2 / §29.3: explicit `e.Current` property access where
    // `e` is typed as the builtin IEnumerator<T> or OwnedEnumerator<T> (no sema
    // TypeDecl, hence no PropertySymbol — lowerVirtualPropertyGet returns null).
    // The iterator state struct's vtable layout is [0]=td, [1]=Dispose,
    // [2]=MoveNext(→i1), [3]=Current(→T). Dispatch through slot 3 returning T.
    // This complements the MoveNext fix in lowerCall (issue #494).
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->member == "Current" && ma->target) {
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
                    llvm::Type* elemTy =
                        (!recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                        ? lowerTypeRef(recvTy->genericArgs[0])
                        : i32();
                    llvm::Value* vtable = builder_.CreateLoad(ptrTy(), recv);
                    auto* currentTy =
                        llvm::FunctionType::get(elemTy, {ptrTy()}, false);
                    llvm::Value* curSlotPtr = builder_.CreateGEP(
                        ptrTy(), vtable, {builder_.getInt32(3)});
                    llvm::Value* curFnPtr =
                        builder_.CreateLoad(ptrTy(), curSlotPtr);
                    return builder_.CreateCall(
                        currentTy, curFnPtr, {recv}, "cur");
                }
            }
        }
    }
    // #481 / §12.1 / §12.5 / §13 / §23: a VIRTUAL or INTERFACE property read
    // through a (potentially) polymorphic receiver dispatches dynamically — load
    // the accessor function from the receiver's vtable/itable and call it. This
    // precedes the static getter paths below (which would bind the *static*
    // type's accessor and miscompile / crash an override).
    if (llvm::Value* v = lowerVirtualPropertyGet(e)) return v;
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
            // #574 / §10.4: enums are value types (i32), but their getter
            // bodies use `this` as a plain integer (e.g. `case this ...`),
            // NOT as a struct pointer. Skip the spill for enum receivers so
            // that `scrut = lowerExpr("this")` inside `lowerCaseExpr` yields
            // the raw i32 value that can be compared against enum constants
            // via icmp. Spilling an i32 to a ptr and then comparing the ptr
            // against an i32 constant would produce malformed IR (icmp with
            // mismatched types → IR verifier failure).
            bool isEnumReceiver = propSym && propSym->parent &&
                                  propSym->parent->kind == ast::TypeDeclKind::Enum;
            if (!obj->getType()->isPointerTy() && !isEnumReceiver) {
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
            // §28.2 v0.134 / issue #601: checked disposal mode guard for
            // expression-bodied property getters inlined at call site. The
            // guard is emitted against the already-loaded `obj` pointer;
            // only class receivers carry a disposed flag (records and enums
            // are value types with no disposal lifetime).
            if (checkedDisposal_ && propSym && propSym->parent &&
                propSym->parent->kind == ast::TypeDeclKind::Class &&
                obj->getType()->isPointerTy()) {
                llvm::StructType* st = classStruct(propSym->parent);
                unsigned fi = disposedFlagIndex(propSym->parent);
                llvm::Value* flagAddr =
                    builder_.CreateStructGEP(st, obj, fi, "cd.flag");
                llvm::Value* disposed = builder_.CreateICmpNE(
                    builder_.CreateLoad(disposedFlagTy(), flagAddr),
                    llvm::ConstantInt::get(disposedFlagTy(), 0), "cd.disposed");
                emitGuardThrow(disposed, "DisposedAccessException");
            }
            // #574 / §10.4 / §19.2 (CG30): an enum computed property whose
            // getter body is `case this Red => …` relies on enumTypeOf() to
            // resolve `this` as the declaring enum type so patternMatches can
            // compare the enum constant patterns. enumTypeOf() uses
            // currentEnumMethod_ for a `this` identifier because sema does not
            // attach an exprType to `this` — the same reason CG30 sets it for
            // enum instance methods. Set it here for the duration of the
            // inline getter body so `case this` inside an enum property works
            // exactly as it does inside an enum method.
            const sema::TypeSymbol* savedEnumMethod = currentEnumMethod_;
            if (propSym && propSym->parent &&
                propSym->parent->kind == ast::TypeDeclKind::Enum)
                currentEnumMethod_ = propSym->parent;
            llvm::Value* v = lowerExpr(*getter);
            currentEnumMethod_ = savedEnumMethod;
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
        // §2.5 / §22.9 (issue #503): imported non-virtual property getter.
        // computedGetterOf and blockGetterOf both returned null because the
        // imported PropertySymbol has no ast::PropertyDecl. Call the external
        // getter function `bz$<Class>$<Prop>$propget` declared by
        // declareImportedPropertyGetters.
        if (auto* ps =
                dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma))) {
            auto git = importedPropGetterFns_.find(ps);
            if (git != importedPropGetterFns_.end() && ma->target) {
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj && obj->getType()->isPointerTy()) {
                    llvm::Value* res = builder_.CreateCall(
                        git->second->getFunctionType(), git->second, {obj});
                    emitPendingCheck();
                    return res;
                }
            }
        }
    }
    // CG8: field read `obj.f`.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        // #548 / stdlib §4.8 v0.135: `entry.Key` / `entry.Value` on a
        // DictionaryEntry<K,V> loop variable. The entry alloca is a
        // {K, V} struct (laid down by lowerDictEntryFor); field 0 is Key,
        // field 1 is Value. DictionaryEntry has no user TypeSymbol, so
        // fieldAddr cannot locate its fields — intercept here first.
        if (ma->target && (ma->member == "Key" || ma->member == "Value")) {
            if (auto* tt = ctx_.exprType(*ma->target)) {
                if (tt->kind == sema::TypeRef::Kind::Named &&
                    !tt->declaration &&
                    tt->importedName == "DictionaryEntry" &&
                    tt->genericArgs.size() >= 2) {
                    // The target must be a local whose alloca holds the
                    // {K,V} struct written by lowerDictEntryFor.
                    if (llvm::AllocaInst* slot = localSlot(*ma->target)) {
                        unsigned fi = ma->member == "Key" ? 0 : 1;
                        sema::TypeRef* fieldTy = tt->genericArgs[fi];
                        llvm::Type* llvmFTy = fieldTy ? lowerTypeRef(fieldTy)
                                                       : ptrTy();
                        // The entry alloca type is a 2-element struct {K, V}.
                        llvm::Type* kTy = lowerTypeRef(tt->genericArgs[0]);
                        llvm::Type* vTy = lowerTypeRef(tt->genericArgs[1]);
                        llvm::StructType* entryStTy =
                            llvm::StructType::get(llvm_, {kTy, vTy});
                        llvm::Value* fieldPtr = builder_.CreateStructGEP(
                            entryStTy, slot, fi, "de.fld");
                        (void)llvmFTy;
                        return builder_.CreateLoad(
                            entryStTy->getElementType(fi), fieldPtr, "de.load");
                    }
                }
                // #628 / stdlib §4.9 v0.135: `e.Key` / `e.Value` on a
                // DrainedEntry<K,V> loop variable (from lowerDictDrainFor).
                // The entry alloca is a {K, V*} struct — same layout as
                // DictionaryEntry: field 0 = Key, field 1 = Value (owned ptr).
                if (tt->kind == sema::TypeRef::Kind::Named &&
                    !tt->declaration &&
                    tt->importedName == "DrainedEntry" &&
                    tt->genericArgs.size() >= 2) {
                    if (llvm::AllocaInst* slot = localSlot(*ma->target)) {
                        unsigned fi = ma->member == "Key" ? 0 : 1;
                        llvm::Type* kTy = lowerTypeRef(tt->genericArgs[0]);
                        llvm::Type* vTy = lowerTypeRef(tt->genericArgs[1]);
                        llvm::StructType* entryStTy =
                            llvm::StructType::get(llvm_, {kTy, vTy});
                        llvm::Value* fieldPtr = builder_.CreateStructGEP(
                            entryStTy, slot, fi, "dde.fld");
                        return builder_.CreateLoad(
                            entryStTy->getElementType(fi), fieldPtr, "dde.load");
                    }
                }
            }
        }
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
        // #625 / §12 / §31: bare `Message` inside a user class that inherits
        // a built-in exception — sema resolved it as the inherited Exception
        // property (no FieldSymbol, type=string).  Emit the same GEP-at-byte-8
        // read as `this.Message` in the qualified MemberAccessExpr path above
        // (CG22+): the message pointer lives at byte offset 8 in the exception
        // object layout regardless of whether the receiver is the builtin class
        // itself or a user-derived class (§31 / classStruct layout rule).
        if (auto* id2 = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            if (id2->name == "Message" &&
                    !ctx_.exprSymbol(e) &&
                    currentThis_) {
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* msgSlot = builder_.CreateGEP(
                    i8Ty, currentThis_, builder_.getInt64(8), "msg.slot");
                llvm::Value* msgPtr =
                    builder_.CreateLoad(ptrTy(), msgSlot, "msg.ptr");
                auto* isNull = builder_.CreateICmpEQ(
                    msgPtr,
                    llvm::ConstantPointerNull::get(ptrTy()), "msg.isnull");
                llvm::Value* emptyStr = lowerStringLiteral("\"\"", false);
                return builder_.CreateSelect(
                    isNull, emptyStr, msgPtr, "msg.val");
            }
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


// #481 / §12.1 / §12.5 / §13 / §23: dynamic dispatch of a virtual/interface
// property GETTER read. Mirrors the virtual/interface METHOD dispatch in
// LlvmBackend_Calls.cpp::emitMethodCall — load the receiver's vtable (class) or
// look up the per-interface itable, GEP to the accessor's slot, load the fn ptr,
// and call `T get(ptr this)`.
llvm::Value* Emitter::lowerVirtualPropertyGet(const ast::Expr& e) {
    // The receiver is either an explicit `obj.P` target, or — for a bare `P`
    // read inside an instance member — the current `this`. A static-property
    // read (receiver is a type name) has no `this`; it is excluded below by the
    // requirement that the lowered receiver be a pointer value.
    const ast::Expr* recvExpr = nullptr;
    sema::PropertySymbol* ps = nullptr;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (!ma->target) return nullptr;
        ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
        recvExpr = ma->target.get();
        // `base.P` (§12.3): bypass dispatch — the static getter path handles it.
        if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(recvExpr))
            if (tid->name == "base") return nullptr;
    } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*id));
        // bare `P` → implicit `this.P`; no recvExpr (currentThis_ is the recv).
    } else {
        return nullptr;
    }
    if (!ps || ps->isIndexer) return nullptr;

    bool isIfaceProp = ps->parent &&
                       ps->parent->kind == ast::TypeDeclKind::Interface;
    auto getIt = propGetSlot_.find(ps);
    auto ifGetIt = ifacePropertyGetIndex_.find(ps);
    if (!isIfaceProp && getIt == propGetSlot_.end()) return nullptr;
    if (isIfaceProp && ifGetIt == ifacePropertyGetIndex_.end()) return nullptr;

    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    // §2.5 / §22.9 / §12.1 (issue #503): for an IMPORTED virtual/override
    // property, ps->decl is null (no ast::PropertyDecl in the importer). The
    // vtable slot was still populated by buildVtable (which ran for imported
    // classes too); we only need to derive the return type, which rides the
    // importedMemberType side table (set by the loader from MetaMember::type).
    llvm::Type* valTy = nullptr;
    if (pd) {
        valTy = lowerType(pd->type.get());
    } else {
        // Imported property: get the resolved type from the side table.
        if (sema::TypeRef* tr = ctx_.importedMemberType(ps))
            valTy = lowerTypeRef(tr);
    }
    if (!valTy) valTy = i32();
    auto* fnTy = llvm::FunctionType::get(valTy, {ptrTy()}, false);

    llvm::Value* recv = recvExpr ? lowerExpr(*recvExpr) : currentThis_;
    // §14.2 (record-conforms-to-interface) + §9.4 (record = value type) /
    // §23 (properties): when a generic method `M<T>(x: T) where T: IFoo` is
    // monomorphized with T = a RECORD and the body reads `x.Prop` (an
    // interface property), sema resolves `x.Prop` to the INTERFACE property
    // `IFoo.Prop`. The receiver `x` is a record VALUE (a struct, not a
    // pointer), so the itable-dispatch path below would either need a pointer
    // receiver or return nullptr at the `!recv->getType()->isPointerTy()`
    // guard — and the fallback paths (computedGetterOf/blockGetterOf) see the
    // INTERFACE property symbol (which has no body) and also return nullptr →
    // the property access silently returns 0. This is the property analog of
    // the record-method devirtualization in lowerCall (commit 738a3aa, §14.2
    // record constraint fix). Since T is concretely known at the
    // monomorphization site, devirtualize: resolve the receiver's concrete
    // type through the active generic substitution; if it is a record, redirect
    // `ps` to the record's own property via classPropertyByName and lower the
    // getter directly (spilling the record value as needed — the same path a
    // non-generic `square.Area` uses). Records are sealed (§9.6), so the
    // implementation is exactly the one declared on the record.
    if (isIfaceProp && recv && !recv->getType()->isPointerTy() && recvExpr) {
        const sema::TypeRef* recvTy = ctx_.exprType(*recvExpr);
        // Resolve a bare type-parameter receiver (`T`, no declaration) through
        // the active substitution to its concrete sema type.
        const sema::TypeRef* concrete = recvTy;
        if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
            !recvTy->declaration && recvTy->astNode &&
            recvTy->astNode->name &&
            recvTy->astNode->name->parts.size() == 1) {
            auto sit = genericSubstSemaTy_.find(
                recvTy->astNode->name->parts[0]);
            if (sit != genericSubstSemaTy_.end() && sit->second)
                concrete = sit->second;
        }
        if (concrete && concrete->declaration &&
            concrete->declaration->kind == ast::TypeDeclKind::Record) {
            // Redirect ps to the record's own implementing property.
            if (sema::PropertySymbol* impl = classPropertyByName(
                    concrete->declaration, ps->name)) {
                // Spill the record value to a stack slot so the getter body
                // can access fields via `this` pointer (§9.2 record storage).
                llvm::Value* obj = recv;
                if (!obj->getType()->isPointerTy()) {
                    llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                    builder_.CreateStore(obj, tmp);
                    obj = tmp;
                }
                // Evaluate the record's own getter with `this` = spilled ptr.
                if (const ast::Expr* getter = computedGetterOf(impl)) {
                    llvm::Value* saved = currentThis_;
                    currentThis_ = obj;
                    llvm::Value* v = lowerExpr(*getter);
                    currentThis_ = saved;
                    return v;
                }
                if (const ast::PropertyAccessor* acc = blockGetterOf(impl)) {
                    auto* ipd = dynamic_cast<const ast::PropertyDecl*>(impl->decl);
                    if (ipd) {
                        llvm::Function* getterFn =
                            ensurePropertyBlockGetter(ipd, acc);
                        llvm::Value* res = builder_.CreateCall(
                            getterFn->getFunctionType(), getterFn, {obj});
                        emitPendingCheck();
                        return res;
                    }
                }
            }
        }
    }
    if (!recv) return nullptr;
    // issue #490 / §14.2: in the UNSPECIALIZED generic template body a type
    // parameter T is erased to i32. A constraint property `x.Name` (x: T)
    // dispatches through an itable/vtable that requires a pointer receiver, but
    // `recv` is i32 here. The record-devirtualization block above (lines
    // 1692-1739) already ran and found no concrete record substitution (the
    // unspecialized body has no active genericSubstSemaTy_), so we fell
    // through. Returning nullptr propagates to lowerLocalDecl, which then picks
    // i32 as the slot type for an inferred-type local `var n := x.Name`; a
    // subsequent `n.Length` or any string/class member call on `n` then emits
    // a malformed IR instruction (string GEP with an i32 base) → LLVM
    // assertion / bezelc crash.
    //
    // Fix: return llvm::UndefValue::get(valTy) — a properly-typed placeholder.
    // This gives lowerLocalDecl the correct slot type (ptr for string, ptr for
    // any class reference) even in the unspecialized stub, so `n.Length` emits
    // well-formed IR. The stub body is never executed (the specialization path
    // in emitGenericSpecialization compiles a concrete version with a real ptr
    // receiver), so the undef value is harmless. This mirrors the #484 guard
    // in emitMethodCall/emitVirtualDispatch (which returns nullptr for a non-ptr
    // receiver, but there the nullptr only affects the call return value, not
    // a local's slot type — the local's slot type is correct because the
    // property-result path was the gap).
    if (!recv->getType()->isPointerTy()) return llvm::UndefValue::get(valTy);

    llvm::Value* fnPtr = nullptr;
    if (isIfaceProp) {
        // CG35-itable (§13): itab lookup then GEP to the property's index.
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        auto* lookupFn = declareC("bz_itable_lookup", ptrTy(),
                                  {ptrTy(), ptrTy()});
        llvm::Value* itmapPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(1)}));
        auto descIt = descArray_.find(ps->parent);
        if (descIt == descArray_.end()) return nullptr;
        llvm::Value* itab = builder_.CreateCall(
            llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
            lookupFn, {itmapPtr, descIt->second}, "itab");
        unsigned k = ifGetIt->second;
        fnPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), itab, {builder_.getInt32(k)}), "pget");
    } else {
        // Class virtual dispatch: vt[slot].
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        llvm::Value* slotPtr = builder_.CreateGEP(
            ptrTy(), vptr, {builder_.getInt32(getIt->second)});
        fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
    }
    llvm::Value* r = builder_.CreateCall(fnTy, fnPtr, {recv});
    emitPendingCheck();  // the accessor body may throw (§23.3)
    return r;
}


// #481: dynamic dispatch of a virtual/interface property SETTER for
// `obj.P := v`, with the already-lowered value `v`. Mirrors the getter dispatch
// above; the call is `void set(ptr this, T value)`.
bool Emitter::lowerVirtualPropertySet(const ast::Expr& target, llvm::Value* v) {
    if (!v) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target);
    if (!ma || !ma->target) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
    if (!ps || ps->isIndexer) return false;
    if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get()))
        if (tid->name == "base") return false;  // §12.3

    bool isIfaceProp = ps->parent &&
                       ps->parent->kind == ast::TypeDeclKind::Interface;
    auto setIt = propSetSlot_.find(ps);
    auto ifSetIt = ifacePropertySetIndex_.find(ps);
    if (!isIfaceProp && setIt == propSetSlot_.end()) return false;
    if (isIfaceProp && ifSetIt == ifacePropertySetIndex_.end()) return false;

    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return false;
    llvm::Type* valTy = lowerType(pd->type.get());
    if (!valTy) valTy = i32();
    auto* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy(), valTy}, false);

    llvm::Value* recv = lowerExpr(*ma->target);
    if (!recv || !recv->getType()->isPointerTy()) return false;
    v = coerceNumericToType(v, valTy, isUnsignedTargetExpr(target));

    llvm::Value* fnPtr = nullptr;
    if (isIfaceProp) {
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        auto* lookupFn = declareC("bz_itable_lookup", ptrTy(),
                                  {ptrTy(), ptrTy()});
        llvm::Value* itmapPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(1)}));
        auto descIt = descArray_.find(ps->parent);
        if (descIt == descArray_.end()) return false;
        llvm::Value* itab = builder_.CreateCall(
            llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
            lookupFn, {itmapPtr, descIt->second}, "itab");
        unsigned k = ifSetIt->second;
        fnPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), itab, {builder_.getInt32(k)}), "pset");
    } else {
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        llvm::Value* slotPtr = builder_.CreateGEP(
            ptrTy(), vptr, {builder_.getInt32(setIt->second)});
        fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
    }
    builder_.CreateCall(fnTy, fnPtr, {recv, v});
    emitPendingCheck();
    return true;
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
        // fix #499 / §9.8: `x in r` where r is a Range-typed variable.
        // Load the stored { i64 start, i64 end, i64 step, i8 flags } struct
        // and compute Contains(x) by endpoint + inclusivity check (§9.8:
        // Step does not affect Contains). Equivalent to the inline-literal
        // fast path above but extracts lo/hi/flags from the struct.
        if (bin.right && !range) {
            auto* rTy = ctx_.exprType(*bin.right);
            if (isRangeType(rTy)) {
                llvm::Value* x = lowerExpr(*bin.left);
                llvm::Value* rv = lowerExpr(*bin.right);
                if (!x || !rv) return nullptr;
                llvm::Value* lo  = builder_.CreateExtractValue(rv, {0u}, "r.lo");
                llvm::Value* hi  = builder_.CreateExtractValue(rv, {1u}, "r.hi");
                llvm::Value* flg = builder_.CreateExtractValue(rv, {3u}, "r.flg");
                // Widen x to i64 for comparison against i64 start/end.
                llvm::Value* xW = x->getType() == i64()
                                      ? x
                                      : builder_.CreateSExt(x, i64(), "r.xw");
                llvm::Value* geL = builder_.CreateICmpSGE(xW, lo, "r.geL");
                // flags bit 0: IsEndInclusive.
                llvm::Value* incl =
                    builder_.CreateTrunc(flg, i1(), "r.incl");
                // Build both branches and select.
                llvm::Value* leLE = builder_.CreateICmpSLE(xW, hi, "r.leLE");
                llvm::Value* leLT = builder_.CreateICmpSLT(xW, hi, "r.leLT");
                llvm::Value* leH  = builder_.CreateSelect(incl, leLE, leLT, "r.leH");
                return builder_.CreateAnd(geL, leH, "r.in");
            }
        }
        // §17.5/§4.6: `c in s` — char membership test in a string.
        // True when the char c appears at any position in s.
        // fix #558 / §17.5/§4.6: `sub in s` — substring containment when LHS is
        // also a string. Lowers to `bz_str_contains(s, sub)` — the same runtime
        // function that backs `s.Contains(sub: string): bool` (§9.6). Both forms
        // require a `string` RHS; they differ only in LHS type (Char vs String).
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            if (rTy && rTy->kind == sema::TypeRef::Kind::Primitive &&
                rTy->primitive == sema::PrimitiveType::String) {
                auto* lTy = ctx_.exprType(*bin.left);
                // §17.5/§4.6 (fix #558): `string in string` — substring test.
                // Reuses bz_str_contains(haystack, needle): i32 which is the same
                // function backing string.Contains(sub: string) (§9.6, #535).
                if (lTy && lTy->kind == sema::TypeRef::Kind::Primitive &&
                    lTy->primitive == sema::PrimitiveType::String) {
                    llvm::Value* needle   = lowerExpr(*bin.left);
                    llvm::Value* haystack = lowerExpr(*bin.right);
                    if (!needle || !haystack) return nullptr;
                    auto* fn = declareC("bz_str_contains", i32(), {ptrTy(), ptrTy()});
                    llvm::Value* res = builder_.CreateCall(fn, {haystack, needle},
                                                           "sin.r");
                    return builder_.CreateTrunc(res, i1(), "sin");
                }
                if (lTy && lTy->kind == sema::TypeRef::Kind::Primitive &&
                    lTy->primitive == sema::PrimitiveType::Char) {
                    // Implement as: scan through string code points for a match.
                    llvm::Value* needle = lowerExpr(*bin.left);
                    llvm::Value* haystack = lowerExpr(*bin.right);
                    if (!needle || !haystack) return nullptr;
                    llvm::Value* len = loadStringLen(haystack);
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
        // fix #524 / §17.5: `elem in set` ≡ set.Contains(elem) for a
        // dict-backed Set<T>.  The right operand must be a Set<T> whose
        // element type has a dict backing (genericBackingType returns non-null);
        // the left operand is the element to look up.
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            bool isSetRHS = rTy && rTy->kind == sema::TypeRef::Kind::Named &&
                            !rTy->declaration && rTy->astNode &&
                            rTy->astNode->name &&
                            !rTy->astNode->name->parts.empty() &&
                            (rTy->astNode->name->parts[0] == "Set" ||
                             rTy->astNode->name->parts[0] == "OwnedSet");
            if (isSetRHS) {
                const sema::TypeRef* elemTy =
                    genericBackingType(rTy, 0, /*isSet=*/true, nullptr);
                if (elemTy) {
                    bool elemIsRecord = recordTypeOfRef(elemTy) != nullptr;
                    llvm::Value* elem = lowerExpr(*bin.left);
                    llvm::Value* set  = lowerExpr(*bin.right);
                    if (!elem || !set) return nullptr;
                    // Spill elem to a stack slot; a record already spilled to ptr.
                    llvm::Value* ep;
                    if (elemIsRecord && elem->getType()->isPointerTy()) {
                        ep = elem;
                    } else {
                        llvm::AllocaInst* slot = entryAlloca(elem->getType());
                        builder_.CreateStore(elem, slot);
                        ep = slot;
                    }
                    auto* fn = declareC("bz_dict_contains_gen", i1(),
                                        {ptrTy(), ptrTy()});
                    return builder_.CreateCall(fn, {set, ep}, "set.in");
                }
            }
        }
        // fix #524 / §4.8: `key in dict` ≡ dict.ContainsKey(key) for a
        // Dictionary<K,V>.  String keys use the dedicated fast path
        // (bz_dict_contains_str); generic keys use bz_dict_contains_gen.
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            if (isDictTypeName(rTy)) {
                const sema::TypeRef* keyTy =
                    genericBackingType(rTy, 0, /*isSet=*/false, nullptr);
                bool keyIsRecord = keyTy && recordTypeOfRef(keyTy) != nullptr;
                llvm::Value* key  = lowerExpr(*bin.left);
                llvm::Value* dict = lowerExpr(*bin.right);
                if (!key || !dict) return nullptr;
                if (keyTy) {
                    // Generic (non-string) key: spill to a slot, pass pointer.
                    llvm::Value* kp;
                    if (keyIsRecord && key->getType()->isPointerTy()) {
                        kp = key;
                    } else {
                        llvm::AllocaInst* slot = entryAlloca(key->getType());
                        builder_.CreateStore(key, slot);
                        kp = slot;
                    }
                    auto* fn = declareC("bz_dict_contains_gen", i1(),
                                        {ptrTy(), ptrTy()});
                    return builder_.CreateCall(fn, {dict, kp}, "dict.in");
                }
                // String key fast path.
                llvm::Value* kdata = loadStringData(key);
                llvm::Value* klen  = loadStringLen(key);
                auto* fn = declareC("bz_dict_contains_str", i1(),
                                    {ptrTy(), ptrTy(), i64()});
                return builder_.CreateCall(fn, {dict, kdata, klen}, "dict.in");
            }
        }
        // §17.5: `x in coll` where `coll` is an Array<T>-, Stack<T>-, or
        // Queue<T>-typed expression (all bz_array_t-backed collections except
        // Set/OwnedSet, which are dict-backed and handled above).
        // Lowers to the same linear scan as Array<T>.Contains(x): iterate via
        // bz_array_len / bz_array_ptr_at and compare each element using
        // emitRemoveElementEquality (record/string/value-type equality per §8.3).
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            // isCollectionTypeName covers Array, OwnedArray, Stack, OwnedStack,
            // Queue, OwnedQueue, Set, OwnedSet — but Set/OwnedSet are already
            // handled above by the dict-backed path; they only reach here when
            // genericBackingType returned null (no generic arg), which is a
            // degenerate case not reached in practice.  isLinkedListTypeName
            // covers LinkedList and OwnedLinkedList (bz_llist_t-backed).
            bool isArr = isCollectionTypeName(rTy);
            bool isLL  = isLinkedListTypeName(rTy);
            if (isArr || isLL) {
                const sema::TypeRef* elemRef =
                    (rTy && !rTy->genericArgs.empty()) ? rTy->genericArgs[0]
                                                       : nullptr;
                bool elemIsClass = collectionElemIsClass(rTy, 0);
                llvm::Value* elem = lowerExpr(*bin.left);
                llvm::Value* coll = lowerExpr(*bin.right);
                if (!elem || !coll) return nullptr;

                if (isLL) {
                    // fix #560 / §17.5/§4.6/§8.3: walk the linked list applying
                    // emitRemoveElementEquality for per-element comparison so that
                    // string content equality (§4.6), record structural equality
                    // (§8.3/§9.1) and class Equals-dispatch (§8.4) are all correct.
                    // The previous bz_llist_find path used memcmp on raw element
                    // bytes — wrong for string elements (compared bz_string* pointer,
                    // not content) and for records with string fields.
                    llvm::Type* elemLLTy = elemRef ? lowerTypeRef(elemRef) : i64();
                    if (!elemLLTy) elemLLTy = i64();
                    auto* firstFn = declareC("bz_llist_first_node", ptrTy(),
                                            {ptrTy()});
                    auto* nextFn  = declareC("bz_llist_node_next", ptrTy(),
                                            {ptrTy()});
                    auto* valFn   = declareC("bz_llist_node_value_ptr", ptrTy(),
                                            {ptrTy()});
                    auto* nullP = llvm::ConstantPointerNull::get(ptrTy());
                    // Spill `elem` (the search target) to a stable slot so that
                    // emitRemoveElementEquality receives consistent operand types.
                    llvm::AllocaInst* tgtSlot = entryAlloca(elem->getType());
                    builder_.CreateStore(elem, tgtSlot);
                    // nodeSlot holds the current node pointer across iterations.
                    llvm::AllocaInst* nodeSlot = entryAlloca(ptrTy());
                    llvm::Value* firstNode = builder_.CreateCall(
                        firstFn, {coll}, "ll.in.first");
                    builder_.CreateStore(firstNode, nodeSlot);
                    auto* condBB  = llvm::BasicBlock::Create(llvm_, "llin.c",  currentFn_);
                    auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "llin.b",  currentFn_);
                    auto* foundBB = llvm::BasicBlock::Create(llvm_, "llin.f",  currentFn_);
                    auto* nextBB  = llvm::BasicBlock::Create(llvm_, "llin.n",  currentFn_);
                    auto* endBB   = llvm::BasicBlock::Create(llvm_, "llin.e",  currentFn_);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(condBB);
                    llvm::Value* curNode = builder_.CreateLoad(ptrTy(), nodeSlot,
                                                               "ll.in.cur");
                    builder_.CreateCondBr(
                        builder_.CreateICmpNE(curNode, nullP, "ll.in.nnull"),
                        bodyBB, endBB);
                    builder_.SetInsertPoint(bodyBB);
                    llvm::Value* vp = builder_.CreateCall(valFn, {curNode},
                                                          "ll.in.vp");
                    llvm::Value* el = builder_.CreateLoad(elemLLTy, vp, "ll.in.el");
                    // Load `elem` back from slot to match the type `el` has.
                    llvm::Value* tgt = builder_.CreateLoad(elem->getType(),
                                                           tgtSlot, "ll.in.tgt");
                    llvm::Value* eq = emitRemoveElementEquality(el, tgt,
                                                                elemRef,
                                                                elemIsClass);
                    if (!eq) return nullptr;
                    builder_.CreateCondBr(eq, foundBB, nextBB);
                    builder_.SetInsertPoint(foundBB);
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(nextBB);
                    llvm::Value* nxt = builder_.CreateCall(nextFn, {curNode},
                                                            "ll.in.next");
                    builder_.CreateStore(nxt, nodeSlot);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(endBB);
                    auto* phi = builder_.CreatePHI(i1(), 2, "llin.result");
                    phi->addIncoming(builder_.getTrue(),  foundBB);
                    phi->addIncoming(builder_.getFalse(), condBB);
                    return phi;
                }

                // Array-backed path: emit the same scan loop as
                // tryLowerCollectionInstanceCall's Contains branch.
                llvm::Type* elemTy = elemRef ? lowerTypeRef(elemRef) : i64();
                if (!elemTy) elemTy = i64();
                int64_t esz = collectionElemSize(rTy);
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* len = builder_.CreateCall(lenFn, {coll}, "coll.len");
                auto* idxSlot = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), idxSlot);
                auto* condBB  = llvm::BasicBlock::Create(llvm_, "cin2.c", currentFn_);
                auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "cin2.b", currentFn_);
                auto* foundBB = llvm::BasicBlock::Create(llvm_, "cin2.f", currentFn_);
                auto* nextBB  = llvm::BasicBlock::Create(llvm_, "cin2.n", currentFn_);
                auto* endBB   = llvm::BasicBlock::Create(llvm_, "cin2.e", currentFn_);
                builder_.CreateBr(condBB);
                builder_.SetInsertPoint(condBB);
                llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
                builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len),
                                      bodyBB, endBB);
                builder_.SetInsertPoint(bodyBB);
                auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                       {ptrTy(), i64(), i64()});
                llvm::Value* ep = builder_.CreateCall(
                    ptrFn, {coll, idx, builder_.getInt64(esz)});
                llvm::Value* el = builder_.CreateLoad(elemTy, ep);
                llvm::Value* eq =
                    emitRemoveElementEquality(el, elem, elemRef, elemIsClass);
                if (!eq) return nullptr;
                builder_.CreateCondBr(eq, foundBB, nextBB);
                builder_.SetInsertPoint(foundBB);
                builder_.CreateBr(endBB);
                builder_.SetInsertPoint(nextBB);
                builder_.CreateStore(
                    builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
                builder_.CreateBr(condBB);
                builder_.SetInsertPoint(endBB);
                auto* phi = builder_.CreatePHI(i1(), 2, "coll.in");
                phi->addIncoming(builder_.getTrue(),  foundBB);
                phi->addIncoming(builder_.getFalse(), condBB);
                return phi;
            }
        }
        // #628 (part 1): unhandled `in` form — emit a hard error so the
        // caller sees a failed compile rather than silent garbage IR.
        ctx_.diagnostics().report(
            Severity::Error, bin.span.begin,
            "not yet implemented: codegen for this `in` expression form");
        return nullptr;
    }
    llvm::Value* l = lowerExpr(*bin.left);
    llvm::Value* r = lowerExpr(*bin.right);
    if (!l || !r) return nullptr;
    const auto& op = bin.op;
    // §7.11 Lifted operators — nullable value types (int?, long?, double?,
    // bool?, …). When either operand is a nullable value type:
    //   - Arithmetic / bitwise / shift: result is T? (null when EITHER operand
    //     is null). The underlying op and its overflow/div-zero checks run ONLY
    //     when both operands are present (conditional branch, NOT a select).
    //   - Relational (<, <=, >, >=): result is bool (false when EITHER null).
    //     fcmp/icmp never trap, so a select suffices.
    // Equality (= / <>) is handled separately (§8.4, already null-safe above).
    // User-defined operator overloads are handled by the early call path above.
    {
        const sema::TypeRef* lTy = bin.left  ? ctx_.exprType(*bin.left)  : nullptr;
        const sema::TypeRef* rTy = bin.right ? ctx_.exprType(*bin.right) : nullptr;
        // §7.11 lifting: detect nullable value type operands.
        // For mixed T + T? where left is non-nullable: lNullable = false but
        // rNullable = true. The left payload is the raw scalar (l itself),
        // not a struct — check isStructTy() guards this case.
        bool lNullable = lTy && isNullableValueTy(lTy) && l->getType()->isStructTy();
        bool rNullable = rTy && isNullableValueTy(rTy) && r->getType()->isStructTy();
        bool isArithOp = (op == "+" || op == "-" || op == "*" || op == "/" ||
                          op == "mod" || op == "&" || op == "|" || op == "^" ||
                          op == "<<" || op == ">>");
        bool isRelOp   = (op == "<" || op == "<=" || op == ">" || op == ">=");
        if ((lNullable || rNullable) && (isArithOp || isRelOp)) {
            // Extract (tag, payload) for each operand.
            // A non-nullable operand has implicit tag=true and its value as payload.
            llvm::Value* lTag     = lNullable ? nullablePrimHasValue(l) : builder_.getTrue();
            llvm::Value* lPayload = lNullable ? nullablePrimValue(l)    : l;
            llvm::Value* rTag     = rNullable ? nullablePrimHasValue(r) : builder_.getTrue();
            llvm::Value* rPayload = rNullable ? nullablePrimValue(r)    : r;
            // bothPresent = lTag AND rTag (i1).
            llvm::Value* bothPresent = builder_.CreateAnd(lTag, rTag, "lift.both");

            // ---- RELATIONAL: result is bool, no trap possible. ----
            if (isRelOp) {
                // Compute the comparison on payloads (safe to do always — icmp/fcmp
                // never trap, and we AND with bothPresent anyway).
                bool lFP = lPayload->getType()->isFloatingPointTy();
                bool rFP = rPayload->getType()->isFloatingPointTy();
                llvm::Value* cmp = nullptr;
                if (lFP || rFP) {
                    // Promote narrower FP type if widths differ.
                    auto* fty =
                        (lFP && rFP)
                            ? (lPayload->getType()->getPrimitiveSizeInBits() >=
                                       rPayload->getType()->getPrimitiveSizeInBits()
                                   ? lPayload->getType()
                                   : rPayload->getType())
                            : (lFP ? lPayload->getType() : rPayload->getType());
                    auto toFP = [&](llvm::Value* v) -> llvm::Value* {
                        if (!v->getType()->isFloatingPointTy())
                            return builder_.CreateSIToFP(v, fty);
                        if (v->getType() != fty)
                            return builder_.CreateFPExt(v, fty);
                        return v;
                    };
                    llvm::Value* lp = toFP(lPayload);
                    llvm::Value* rp = toFP(rPayload);
                    if      (op == "<")  cmp = builder_.CreateFCmpOLT(lp, rp, "lift.cmp");
                    else if (op == "<=") cmp = builder_.CreateFCmpOLE(lp, rp, "lift.cmp");
                    else if (op == ">")  cmp = builder_.CreateFCmpOGT(lp, rp, "lift.cmp");
                    else                 cmp = builder_.CreateFCmpOGE(lp, rp, "lift.cmp");
                } else {
                    // Integer comparison; widen to common width if needed.
                    llvm::Value* lp = lPayload, *rp = rPayload;
                    if (lp->getType()->isIntegerTy() && rp->getType()->isIntegerTy() &&
                        lp->getType() != rp->getType()) {
                        unsigned lb = lp->getType()->getIntegerBitWidth();
                        unsigned rb = rp->getType()->getIntegerBitWidth();
                        if (lb < rb) lp = builder_.CreateSExt(lp, rp->getType());
                        else         rp = builder_.CreateSExt(rp, lp->getType());
                    }
                    bool uns = isUnsignedBinOperand(bin);
                    if      (op == "<")  cmp = uns ? builder_.CreateICmpULT(lp, rp, "lift.cmp")
                                                   : builder_.CreateICmpSLT(lp, rp, "lift.cmp");
                    else if (op == "<=") cmp = uns ? builder_.CreateICmpULE(lp, rp, "lift.cmp")
                                                   : builder_.CreateICmpSLE(lp, rp, "lift.cmp");
                    else if (op == ">")  cmp = uns ? builder_.CreateICmpUGT(lp, rp, "lift.cmp")
                                                   : builder_.CreateICmpSGT(lp, rp, "lift.cmp");
                    else                 cmp = uns ? builder_.CreateICmpUGE(lp, rp, "lift.cmp")
                                                   : builder_.CreateICmpSGE(lp, rp, "lift.cmp");
                }
                // §7.11: false when either operand is null; inner cmp when both present.
                return builder_.CreateAnd(bothPresent, cmp, "lift.rel");
            }

            // ---- ARITHMETIC / BITWISE / SHIFT: result is T? ----
            // The underlying op (including overflow/div-zero checks) must run
            // ONLY in the bothPresent branch (§7.11). Use a conditional branch
            // so the side-effecting checks don't fire on null-by-null-operand paths.
            // Determine the result sema type (T?) from whichever operand is nullable.
            const sema::TypeRef* nvTy = lNullable ? lTy : rTy;
            // Build the conditional branch.
            auto* entryBB  = builder_.GetInsertBlock();
            auto* opBB     = llvm::BasicBlock::Create(llvm_, "lift.op",  currentFn_);
            auto* mergeBB  = llvm::BasicBlock::Create(llvm_, "lift.end", currentFn_);
            builder_.CreateCondBr(bothPresent, opBB, mergeBB);

            // --- opBB: both operands are present, run the actual op. ---
            builder_.SetInsertPoint(opBB);
            // Widen integer payloads to a common width (mirrors the non-nullable path).
            llvm::Value* lp = lPayload, *rp = rPayload;
            bool lFP = lp->getType()->isFloatingPointTy();
            bool rFP = rp->getType()->isFloatingPointTy();
            if (!lFP && !rFP && lp->getType()->isIntegerTy() &&
                rp->getType()->isIntegerTy() &&
                lp->getType() != rp->getType()) {
                bool uns = isUnsignedBinOperand(bin);
                unsigned lb = lp->getType()->getIntegerBitWidth();
                unsigned rb = rp->getType()->getIntegerBitWidth();
                llvm::Type* wide = lb >= rb ? lp->getType() : rp->getType();
                if (lb < rb) lp = uns ? builder_.CreateZExt(lp, wide)
                                      : builder_.CreateSExt(lp, wide);
                if (rb < lb) rp = uns ? builder_.CreateZExt(rp, wide)
                                      : builder_.CreateSExt(rp, wide);
            }
            // Promote integer payload to FP when the other side is FP.
            if (lFP || rFP) {
                auto* fty =
                    (lFP && rFP)
                        ? (lp->getType()->getPrimitiveSizeInBits() >=
                                   rp->getType()->getPrimitiveSizeInBits()
                               ? lp->getType()
                               : rp->getType())
                        : (lFP ? lp->getType() : rp->getType());
                auto toFP = [&](llvm::Value* v) -> llvm::Value* {
                    if (!v->getType()->isFloatingPointTy())
                        return builder_.CreateSIToFP(v, fty);
                    if (v->getType() != fty)
                        return builder_.CreateFPExt(v, fty);
                    return v;
                };
                lp = toFP(lp); rp = toFP(rp);
            }
            // Compute the payload result using the same logic as the non-nullable
            // path (including overflow and div-zero checks, per §7.11).
            llvm::Value* payloadResult = nullptr;
            if (lp->getType()->isFloatingPointTy()) {
                // FP arithmetic — no overflow/div-zero checks (§6.3).
                if      (op == "+")   payloadResult = builder_.CreateFAdd(lp, rp);
                else if (op == "-")   payloadResult = builder_.CreateFSub(lp, rp);
                else if (op == "*")   payloadResult = builder_.CreateFMul(lp, rp);
                else if (op == "/")   payloadResult = builder_.CreateFDiv(lp, rp);
            } else {
                bool uns = isUnsignedBinOperand(bin);
                // CG22 checked integer arithmetic (§6.1) — overflow/div-zero
                // checks fire here, inside the bothPresent branch.
                bool checkedInt = lp->getType()->isIntegerTy() &&
                                  !lp->getType()->isIntegerTy(1);
                if (checkedInt) {
                    if (op == "+") {
                        payloadResult = emitCheckedArith(
                            uns ? llvm::Intrinsic::uadd_with_overflow
                                : llvm::Intrinsic::sadd_with_overflow, lp, rp);
                    } else if (op == "-") {
                        payloadResult = emitCheckedArith(
                            uns ? llvm::Intrinsic::usub_with_overflow
                                : llvm::Intrinsic::ssub_with_overflow, lp, rp);
                    } else if (op == "*") {
                        payloadResult = emitCheckedArith(
                            uns ? llvm::Intrinsic::umul_with_overflow
                                : llvm::Intrinsic::smul_with_overflow, lp, rp);
                    } else if (op == "/" || op == "mod") {
                        emitGuardThrow(builder_.CreateICmpEQ(rp, zeroLike(rp)),
                                       "DivideByZeroException");
                        if (uns) {
                            payloadResult = (op == "/") ? builder_.CreateUDiv(lp, rp)
                                                        : builder_.CreateURem(lp, rp);
                        } else {
                            llvm::Value* minV   = minIntLike(rp);
                            llvm::Value* negOne = llvm::ConstantInt::getSigned(
                                rp->getType(), -1);
                            emitGuardThrow(
                                builder_.CreateAnd(builder_.CreateICmpEQ(lp, minV),
                                                   builder_.CreateICmpEQ(rp, negOne)),
                                "OverflowException");
                            payloadResult = (op == "/") ? builder_.CreateSDiv(lp, rp)
                                                        : builder_.CreateSRem(lp, rp);
                        }
                    }
                }
                if (!payloadResult) {
                    // Unchecked ops (bool bitwise on i1, shifts, plain add/sub for
                    // non-integer — fallback).
                    if      (op == "+")   payloadResult = builder_.CreateAdd(lp, rp);
                    else if (op == "-")   payloadResult = builder_.CreateSub(lp, rp);
                    else if (op == "*")   payloadResult = builder_.CreateMul(lp, rp);
                    else if (op == "/")   payloadResult = uns ? builder_.CreateUDiv(lp, rp)
                                                              : builder_.CreateSDiv(lp, rp);
                    else if (op == "mod") payloadResult = uns ? builder_.CreateURem(lp, rp)
                                                              : builder_.CreateSRem(lp, rp);
                    else if (op == "&")   payloadResult = builder_.CreateAnd(lp, rp);
                    else if (op == "|")   payloadResult = builder_.CreateOr(lp, rp);
                    else if (op == "^")   payloadResult = builder_.CreateXor(lp, rp);
                    else if (op == "<<" || op == ">>") {
                        unsigned bits = lp->getType()->getIntegerBitWidth();
                        llvm::Value* mask = llvm::ConstantInt::get(rp->getType(), bits - 1);
                        llvm::Value* cnt = builder_.CreateAnd(rp, mask, "shcnt");
                        if (op == "<<")  payloadResult = builder_.CreateShl(lp, cnt);
                        else             payloadResult = uns ? builder_.CreateLShr(lp, cnt)
                                                             : builder_.CreateAShr(lp, cnt);
                    }
                }
            }
            auto* opEndBB = builder_.GetInsertBlock();
            builder_.CreateBr(mergeBB);

            // --- mergeBB: phi between present-value and zero-payload. ---
            builder_.SetInsertPoint(mergeBB);
            // PHI nodes must be first in the block. Create the payload PHI first,
            // then the tag ZExt (which uses bothPresent from entryBB — a constant-
            // or instruction-value already computed before the branch, so it is
            // dominating and valid as a non-PHI operand after the PHIs).
            // Build the result T? struct: {i8 tag, T payload}.
            // - tag: 1 when both present (payloadResult is valid), 0 otherwise.
            // - payload: payloadResult when present, else zero (the tag signals
            //   null so the zero value is never read by correct code).
            llvm::Value* payloadPhi = nullptr;
            if (payloadResult) {
                // PHI first (required to be at the top of the block).
                auto* phi = builder_.CreatePHI(payloadResult->getType(), 2,
                                               "lift.payload");
                phi->addIncoming(payloadResult, opEndBB);
                phi->addIncoming(
                    llvm::Constant::getNullValue(payloadResult->getType()), entryBB);
                payloadPhi = phi;
            } else {
                // Fallback: use zero of the struct's payload type.
                auto* nvSt = nullableValueStructTy(nvTy);
                payloadPhi = llvm::Constant::getNullValue(nvSt->getElementType(1));
            }
            // Non-PHI instructions follow the PHI group.
            llvm::Value* resultTag = builder_.CreateZExt(
                bothPresent, nullablePrimHasValTy(), "lift.tag");
            // Construct the result nullable struct {tag, payload}.
            auto* st = nullableValueStructTy(nvTy);
            llvm::Value* res = builder_.CreateInsertValue(
                llvm::UndefValue::get(st), resultTag, {0u}, "lift.res");
            res = builder_.CreateInsertValue(res, payloadPhi, {1u});
            return res;
        }
    }
    // §4.4 / §6.4 fix #521: decimal arithmetic — route via bz_decimal_* runtime.
    // Decimal values are { i32 lo32, i32 mid32, i32 hi32, i32 flags } structs.
    // All runtime functions take pointers, so we alloca the operands, store,
    // pass their addresses, then load the result.
    {
        using P = sema::PrimitiveType;
        const sema::TypeRef* lTy = bin.left  ? ctx_.exprType(*bin.left)  : nullptr;
        const sema::TypeRef* rTy = bin.right ? ctx_.exprType(*bin.right) : nullptr;
        bool lDec = lTy && lTy->kind == sema::TypeRef::Kind::Primitive &&
                    lTy->primitive == P::Decimal;
        bool rDec = rTy && rTy->kind == sema::TypeRef::Kind::Primitive &&
                    rTy->primitive == P::Decimal;
        // §5.1: integers implicitly widen to decimal — promote an integer operand.
        // We detect this by: result type is decimal, and one operand is integer.
        const sema::TypeRef* resTy = ctx_.exprType(bin);
        bool resDec = resTy && resTy->kind == sema::TypeRef::Kind::Primitive &&
                      resTy->primitive == P::Decimal;
        if (lDec || rDec || resDec) {
            llvm::StructType* dst = decimalStructType();
            // Helper: promote value to decimal struct (alloca + runtime call).
            auto toDecSlot = [&](llvm::Value* v,
                                 const sema::TypeRef* ty) -> llvm::Value* {
                if (ty && ty->kind == sema::TypeRef::Kind::Primitive &&
                    ty->primitive == P::Decimal)
                    return v;  // already decimal struct
                // Integer widening to decimal (§5.1).
                llvm::AllocaInst* slot = entryAlloca(dst);
                if (ty && ty->kind == sema::TypeRef::Kind::Primitive) {
                    P p = ty->primitive;
                    llvm::Value* i64v = v;
                    if (v->getType()->isIntegerTy(64)) {
                        auto* fn = declareC("bz_decimal_from_int64",
                                           llvm::Type::getVoidTy(llvm_),
                                           {i64(), ptrTy()});
                        builder_.CreateCall(fn, {i64v, slot});
                    } else {
                        // Widen to i32 then call from_int32.
                        if (!v->getType()->isIntegerTy(32))
                            i64v = (p == P::Byte || p == P::UShort || p == P::UInt ||
                                    p == P::Char)
                                       ? builder_.CreateZExt(v, i32())
                                       : builder_.CreateSExt(v, i32());
                        auto* fn = declareC("bz_decimal_from_int32",
                                           llvm::Type::getVoidTy(llvm_),
                                           {i32(), ptrTy()});
                        builder_.CreateCall(fn, {i64v, slot});
                    }
                }
                return builder_.CreateLoad(dst, slot, "dec.promoted");
            };
            llvm::Value* lv = toDecSlot(l, lTy);
            llvm::Value* rv = toDecSlot(r, rTy);

            // Arithmetic ops: store operands into alloca slots, call runtime,
            // load result.  §6.4: div/mod are guarded against zero divisor.
            if (op == "+" || op == "-" || op == "*" ||
                op == "/" || op == "mod") {
                // §6.4: check divisor for div/mod.
                if (op == "/" || op == "mod") {
                    // Zero check: mantissa = lo32 | mid32 | hi32.
                    llvm::Value* lo  = builder_.CreateExtractValue(rv, {0u});
                    llvm::Value* mid = builder_.CreateExtractValue(rv, {1u});
                    llvm::Value* hi  = builder_.CreateExtractValue(rv, {2u});
                    llvm::Value* orv = builder_.CreateOr(
                        builder_.CreateOr(lo, mid), hi);
                    llvm::Value* isZero = builder_.CreateICmpEQ(
                        orv, llvm::ConstantInt::get(i32(), 0));
                    emitGuardThrow(isZero, "DivideByZeroException");
                }
                llvm::AllocaInst* aSlot = entryAlloca(dst);
                llvm::AllocaInst* bSlot = entryAlloca(dst);
                llvm::AllocaInst* oSlot = entryAlloca(dst);
                builder_.CreateStore(lv, aSlot);
                builder_.CreateStore(rv, bSlot);
                const char* fn_name =
                    op == "+"   ? "bz_decimal_add" :
                    op == "-"   ? "bz_decimal_sub" :
                    op == "*"   ? "bz_decimal_mul" :
                    op == "/"   ? "bz_decimal_div" :
                    /* mod */     "bz_decimal_mod";
                auto* fn = declareC(fn_name, llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {aSlot, bSlot, oSlot});
                return builder_.CreateLoad(dst, oSlot, "dec.res");
            }
            // Negation (unary - on decimal is handled separately via lowerUnary,
            // but mixed decimal comparisons are the binary ops below).
            // Comparisons: call bz_decimal_cmp, compare result with 0.
            if (op == "=" || op == "<>" ||
                op == "<" || op == "<=" || op == ">" || op == ">=") {
                llvm::AllocaInst* aSlot = entryAlloca(dst);
                llvm::AllocaInst* bSlot = entryAlloca(dst);
                builder_.CreateStore(lv, aSlot);
                builder_.CreateStore(rv, bSlot);
                auto* cmpFn = declareC("bz_decimal_cmp", i32(),
                                       {ptrTy(), ptrTy()});
                llvm::Value* cmp = builder_.CreateCall(cmpFn, {aSlot, bSlot},
                                                       "dec.cmp");
                llvm::Value* zero = llvm::ConstantInt::get(i32(), 0);
                if (op == "=")  return builder_.CreateICmpEQ(cmp, zero);
                if (op == "<>") return builder_.CreateICmpNE(cmp, zero);
                if (op == "<")  return builder_.CreateICmpSLT(cmp, zero);
                if (op == "<=") return builder_.CreateICmpSLE(cmp, zero);
                if (op == ">")  return builder_.CreateICmpSGT(cmp, zero);
                /* >= */        return builder_.CreateICmpSGE(cmp, zero);
            }
        }
    }
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
        // §4.3.1 (L308/L314): "NaN = NaN is true" — reflexive equality.
        // Emit: oeq(a,b) OR (isNaN(a) AND isNaN(b)).
        // isNaN(x) is fcmp uno x, x (unordered compare: true iff x is NaN).
        // IEEE <,<=,>,>= are NOT changed (NaN comparisons stay false, §4.3.1).
        if (op == "=") {
            llvm::Value* ieee_eq  = builder_.CreateFCmpOEQ(l, r, "feq");
            llvm::Value* l_is_nan = builder_.CreateFCmpUNO(l, l, "lnan");
            llvm::Value* r_is_nan = builder_.CreateFCmpUNO(r, r, "rnan");
            llvm::Value* both_nan = builder_.CreateAnd(l_is_nan, r_is_nan, "bnan");
            return builder_.CreateOr(ieee_eq, both_nan, "feq_ref");
        }
        // §4.3.1: `<>` is the negation of `=` (which is true for NaN=NaN),
        // so `NaN <> NaN` is false.  Emit NOT(oeq OR both-NaN).
        if (op == "<>") {
            llvm::Value* ieee_eq  = builder_.CreateFCmpOEQ(l, r, "feq");
            llvm::Value* l_is_nan = builder_.CreateFCmpUNO(l, l, "lnan");
            llvm::Value* r_is_nan = builder_.CreateFCmpUNO(r, r, "rnan");
            llvm::Value* both_nan = builder_.CreateAnd(l_is_nan, r_is_nan, "bnan");
            llvm::Value* eq_ref   = builder_.CreateOr(ieee_eq, both_nan, "feq_ref");
            return builder_.CreateNot(eq_ref, "fne_ref");
        }
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
        // §6.5: shift count is reduced modulo the operand's bit width.
        // LLVM shl/lshr/ashr with count >= bitwidth produces poison; mask
        // the count to [0, bitwidth-1] before emitting the shift.
        if (op == "<<" || op == ">>") {
            unsigned bits = l->getType()->getIntegerBitWidth();
            llvm::Value* mask = llvm::ConstantInt::get(r->getType(), bits - 1);
            llvm::Value* cnt = builder_.CreateAnd(r, mask, "shcnt");
            if (op == "<<")  return builder_.CreateShl(l, cnt);
            return uns ? builder_.CreateLShr(l, cnt)
                       : builder_.CreateAShr(l, cnt);
        }
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
        const sema::TypeRef* recTyRef = nullptr;
        for (const ast::Expr* side : {bin.left.get(), bin.right.get()}) {
            if (!side) continue;
            if (auto* t = ctx_.exprType(*side))
                if (t->kind == sema::TypeRef::Kind::Named && t->declaration &&
                    t->declaration->kind == ast::TypeDeclKind::Record) {
                    recTy = t->declaration; recTyRef = t; break;
                }
        }
        // issue #462 / §8.3 / §14.1: for a GENERIC record (`Box<string> =
        // Box<string>`) install the operand's concrete type args so the
        // synthesized per-field equality resolves `T` → its concrete type (the
        // string field compares by CONTENT, not pointer identity). A no-op for a
        // non-generic record. The operand values already carry the specialized
        // struct shape (construction is specialized since #460), so only the
        // per-field declared-type resolution needs the subst here.
        RecordSubstGuard eqSubst(this, recTyRef);
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

    // §7.11 Lifted unary operators: `-` and `~` on nullable value types.
    // When the operand is a nullable value type (int?, long?, …):
    //   - Result is T? (null when operand is null).
    //   - The underlying op (incl. OverflowException for `-`) runs ONLY when
    //     the operand is present (conditional branch).
    if ((op == "-" || op == "~") && un.operand) {
        const sema::TypeRef* ot = ctx_.exprType(*un.operand);
        if (ot && isNullableValueTy(ot) && v->getType()->isStructTy()) {
            llvm::Value* tag     = nullablePrimHasValue(v);
            llvm::Value* payload = nullablePrimValue(v);
            auto* entryBB = builder_.GetInsertBlock();
            auto* opBB    = llvm::BasicBlock::Create(llvm_, "uneg.op",  currentFn_);
            auto* mergeBB = llvm::BasicBlock::Create(llvm_, "uneg.end", currentFn_);
            builder_.CreateCondBr(tag, opBB, mergeBB);

            builder_.SetInsertPoint(opBB);
            llvm::Value* result = nullptr;
            if (op == "-") {
                if (payload->getType()->isFloatingPointTy()) {
                    result = builder_.CreateFNeg(payload);
                } else {
                    if (payload->getType()->isIntegerTy() &&
                        !payload->getType()->isIntegerTy(1)) {
                        emitGuardThrow(
                            builder_.CreateICmpEQ(payload, minIntLike(payload)),
                            "OverflowException");
                    }
                    result = builder_.CreateNeg(payload);
                }
            } else { // op == "~"
                result = builder_.CreateNot(payload);
            }
            auto* opEndBB = builder_.GetInsertBlock();
            builder_.CreateBr(mergeBB);

            builder_.SetInsertPoint(mergeBB);
            // PHI first (required to be at top of block).
            auto* phi = builder_.CreatePHI(payload->getType(), 2, "uneg.val");
            phi->addIncoming(llvm::Constant::getNullValue(payload->getType()),
                             entryBB);
            phi->addIncoming(result, opEndBB);
            llvm::Value* resTag = builder_.CreateZExt(
                tag, nullablePrimHasValTy(), "uneg.tag");
            auto* st = nullableValueStructTy(ot);
            llvm::Value* res = builder_.CreateInsertValue(
                llvm::UndefValue::get(st), resTag, {0u});
            res = builder_.CreateInsertValue(res, phi, {1u});
            return res;
        }
    }

    if (op == "-") {
        // §4.4 fix #521: decimal negation — flip bit 31 of flags (sign bit).
        if (auto* st = llvm::dyn_cast<llvm::StructType>(v->getType())) {
            if (st == decimalStructType()) {
                llvm::AllocaInst* slot = entryAlloca(st);
                llvm::AllocaInst* out  = entryAlloca(st);
                builder_.CreateStore(v, slot);
                auto* fn = declareC("bz_decimal_neg", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {slot, out});
                return builder_.CreateLoad(st, out, "dec.neg");
            }
        }
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
                // per §14.1/§14.2 (issue #505): if lowerTypeRef still returns
                // i32 (unresolved T from a method-level generic param) but the
                // genericSubst_ has an explicit mapping for this param name
                // (populated by the pre-pass in lowerCall), use that mapping
                // directly. This handles the case where the pre-pass set T=ptr
                // but lowerTypeRef returned i32 because the generic param
                // TypeRef used a different AST node than what genericSubst_ was
                // keyed on — a safety net for the nominal path.
                if (expected == i32() && !genericSubst_.empty() &&
                    expRT->kind == sema::TypeRef::Kind::Named &&
                    !expRT->declaration && expRT->astNode &&
                    expRT->astNode->name &&
                    expRT->astNode->name->parts.size() == 1) {
                    auto gsit = genericSubst_.find(expRT->astNode->name->parts[0]);
                    if (gsit != genericSubst_.end() && gsit->second != i32())
                        expected = gsit->second;
                }
                if (expected != i32()) {
                    ty = expected;
                    // Backfill: give the ParameterSymbol a resolved sema type so
                    // that body identifier references (e.g. `x.v` — a field on
                    // the lambda param) pick up the right TypeRef. Prefer the
                    // CONCRETE TypeRef from genericSubstSemaTy_ over the bare
                    // unresolved generic param TypeRef (expRT): with an unresolved
                    // T TypeRef as the hint, lowerExpr(x) would set
                    // ctx_.exprType(x) = T (no declaration) — and then
                    // ctx_.exprSymbol(x.v) stays null (sema couldn't resolve .v
                    // on an unresolved T) → fieldAddr returns null → ret void in
                    // an i32 function → IR verification failure (#505).
                    // With the concrete A TypeRef, lowerExpr(x) injects A's type
                    // so the MemberAccess resolution chain can proceed.
                    if (par && !par->declType) {
                        sema::TypeRef* hintTy = expRT;
                        // Resolve an unresolved generic param name through the
                        // active sema substitution to get the concrete TypeRef.
                        // genericSubstSemaTy_ stores const TypeRef*; the
                        // TypeRef lives in the SemaContext arena, which is not
                        // logically modified by the lookup — const_cast is safe.
                        if (expRT->kind == sema::TypeRef::Kind::Named &&
                            !expRT->declaration &&
                            expRT->astNode && expRT->astNode->name &&
                            expRT->astNode->name->parts.size() == 1 &&
                            !genericSubstSemaTy_.empty()) {
                            auto sit = genericSubstSemaTy_.find(
                                expRT->astNode->name->parts[0]);
                            if (sit != genericSubstSemaTy_.end() &&
                                sit->second)
                                hintTy = const_cast<sema::TypeRef*>(sit->second);
                        }
                        lambdaParamTypeHint_[par] = hintTy;
                    }
                } else if (par && !par->declType) {
                    // per §14.1 (issue #516): even when the formal param type
                    // resolves to i32 (e.g. `int` in `fn:(int)->U`), backfill
                    // the lambda-param TypeRef hint so that body references like
                    // `x.ToString()` see `int` as the receiver type and take
                    // the primitive dispatch path. Without this, the lambda param
                    // `x` has no sema type at all (sema left it null for an
                    // unannotated param with no event-handler context), and
                    // `x.ToString()` in the body falls to the `!calleeSym` catch
                    // (no MethodSymbol for int.ToString on an untyped receiver)
                    // which returns nullptr → the lambda closure emits `ret void`
                    // in a `ptr`-returning function → IR verification failure.
                    // The LLVM type `ty` stays i32 (correct for int), but sema's
                    // type-oracle (`ctx_.exprType(x)`) now returns `int TypeRef`
                    // so the primitive-intrinsic branch at lowerExpr line ~447
                    // correctly dispatches `x.ToString()`.
                    // Only apply this to CONCRETE (non-generic, non-null) TypeRefs
                    // to avoid injecting an unresolved generic placeholder.
                    if (expRT->kind == sema::TypeRef::Kind::Primitive &&
                        expRT->primitive != sema::PrimitiveType::None) {
                        lambdaParamTypeHint_[par] = expRT;
                    }
                }
            }
            userParamTys.push_back(ty);
            ++pi;
        }
    }

    // Pre-determine return type from sema's exprType of the body.
    // retSemaTy tracks the sema TypeRef that SET retTy (used below for
    // nullable-value promotion in the expr-body emit path — fix #541 / §7).
    llvm::Type* retTy = i32();
    const sema::TypeRef* retSemaTy = nullptr;
    if (lam.exprBody) {
        if (sema::TypeRef* rt = ctx_.exprType(*lam.exprBody)) {
            retTy = lowerTypeRef(rt);
            retSemaTy = rt;
        }
    } else {
        // BUG-L3 / §27: for block-body lambdas the return type is NOT the
        // expression-body type (there is none) but the sema-inferred funcReturn
        // stored in the lambda's own exprType. Sema scans block-body stmts for
        // `return <expr>` to infer the return type; failing that it falls back
        // to `int`. If we leave retTy as i32() unconditionally here, a lambda
        // that explicitly returns `bool` will be typed `i32` by codegen while
        // lowerScopedBlock emits `ret i1` for `return true` — a type mismatch
        // the IR verifier flags ("Function return type does not match operand
        // type of return inst"). Fix: consult the lambda's sema exprType.
        if (sema::TypeRef* lamTy = ctx_.exprType(lam)) {
            if (lamTy->kind == sema::TypeRef::Kind::Function &&
                lamTy->funcReturn) {
                llvm::Type* inferred = lowerTypeRef(lamTy->funcReturn);
                if (inferred != i32()) {
                    retTy = inferred;
                    retSemaTy = lamTy->funcReturn;
                }
            }
        }
    }
    // If return type is still i32 (unresolved generic), try the expected
    // function type's return type (from the call site context).
    if (retTy == i32() && expectedFunctionType_ &&
        expectedFunctionType_->funcReturn) {
        llvm::Type* expected = lowerTypeRef(expectedFunctionType_->funcReturn);
        if (expected != i32()) {
            retTy = expected;
            retSemaTy = expectedFunctionType_->funcReturn;
        }
    }
    // fix #543 / §7 / §17.14: when expectedFunctionType_ declares a NULLABLE
    // VALUE return type (T?), but the body-inferred retTy is NOT the matching
    // tagged struct (e.g. it is the bare underlying record struct or a reference
    // ptr), override retTy with the correct nullable struct.
    //
    // Root cause: sema infers the lambda's funcReturn from the first visible
    // `return <expr>` in the body — for `return new Point(x,x)` this is `Point`
    // (non-nullable), so `lowerTypeRef` returns `%record.Point`. Since
    // `%record.Point != i32()`, the i32-only override above doesn't fire, and
    // the lambda fn is declared to return `%record.Point` instead of
    // `{i8,%record.Point}`. A subsequent `return null;` then emits `ret ptr null`
    // into a `%record.Point`-returning function → IR verification failure.
    //
    // The same pattern applies to BUG-1 (conditional expr body typed Object? →
    // ptr by sema when a null branch exists but the result is T?): the body-
    // inferred type is `ptr` ≠ `i32`, so the existing override skips it too.
    //
    // Fix: when expectedFunctionType_->funcReturn IS a nullable value type, it
    // is authoritative — use it even when a body-derived retTy was already found.
    // The body-derived value is still lowered correctly (the nullable struct is
    // produced by lowerConditional for expr-body, or by emitReturn's coercion for
    // block-body); only the function's declared return type needs correction here.
    // per §7.1 / §7.2 / §17.14.
    if (expectedFunctionType_ && expectedFunctionType_->funcReturn &&
        isNullableValueTy(expectedFunctionType_->funcReturn)) {
        llvm::Type* expected = lowerTypeRef(expectedFunctionType_->funcReturn);
        if (expected != retTy) {
            retTy = expected;
            retSemaTy = expectedFunctionType_->funcReturn;
        }
    }
    // Also try to resolve via genericSubst_ when the sema return type is
    // an unresolved named generic param (e.g. C in Curried<A,B,C>).
    if (retTy == i32() && lam.exprBody) {
        if (sema::TypeRef* rt = ctx_.exprType(*lam.exprBody)) {
            if (rt->kind == sema::TypeRef::Kind::Named && !rt->declaration &&
                rt->astNode && rt->astNode->name &&
                rt->astNode->name->parts.size() == 1) {
                auto sit = genericSubst_.find(rt->astNode->name->parts[0]);
                if (sit != genericSubst_.end() && sit->second != i32()) {
                    retTy = sit->second;
                    // retSemaTy stays the unresolved generic ref — no
                    // nullable-value wrap needed for generic param types.
                }
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
    auto savedFinallyStack = finallyStack_;  // #546 / §31.2: a lambda compiled
        // inside a try-with-finally must not clear the enclosing finally stack
        // permanently — save it here and restore after the lambda body so the
        // enclosing lowerTry's pop_back() finds the frame it pushed.
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
        // BUG-L2 / §27.5: for-var loop variables (decl is a ForStmt) use fresh
        // boxing each iteration — the incBB updates the original slot so each
        // new cell correctly captures the current iteration's value (§16.5 fresh
        // binding). For all other captured locals, use guarded boxing that
        // allocates the cell exactly once (on first capture) and reuses it on
        // subsequent iterations, so the accumulated value is never overwritten by
        // a re-initialisation from the (potentially stale) original slot.
        bool isForVar = dynamic_cast<const ast::ForStmt*>(decl) != nullptr;
        ce.envValue = boxLocalSlot(slot, newSlot, /*forLoopVar=*/isForVar);
        locals_[decl] = newSlot;  // rebind to the ptr cell-holder
        rebindDisposalSlot(slot, newSlot);
        ce.sym = nullptr;
        ce.decl = decl;
        captures.push_back(ce);
    }

    // BUG-L2 / §27.5: boxing non-ForVar locals with the guarded path introduces
    // new basic blocks (a conditional branch in boxLocalSlot).  Update savedBB
    // so the restore below positions the builder at the post-boxing block, not
    // the now-terminated pre-boxing block.
    savedBB = builder_.GetInsertBlock();

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

    // §30.6 (#488): an `async` lambda's closure entry function ALWAYS returns a
    // Task pointer (the body's natural result is the awaited value T, which the
    // state machine wraps into the outer Task — see emitAsyncLambda). Override
    // the body-derived retTy to `ptr` so the closure's fn type matches what
    // emitClosureCall expects (the declared `(…) -> Task<T>`). The element type T
    // (for completing the task with a value) comes from the call-site expected
    // function type's `Task<T>` return annotation; null means a no-value `Task`.
    const sema::TypeRef* asyncResultElemSemaTy = nullptr;
    if (lam.isAsync) {
        bool haveAnnotation =
            expectedFunctionType_ && expectedFunctionType_->funcReturn &&
            expectedFunctionType_->funcReturn->kind ==
                sema::TypeRef::Kind::Named;
        if (haveAnnotation) {
            // The annotation IS authoritative for the void-vs-value distinction
            // (§30.6): a bare `Task` (no generic arg) is a no-value task — leave
            // T null; `Task<T>` / `OwnedTask<T>` carries the element type T.
            const sema::TypeRef* taskTy = expectedFunctionType_->funcReturn;
            if (!taskTy->genericArgs.empty() && taskTy->genericArgs[0])
                asyncResultElemSemaTy = taskTy->genericArgs[0];
        } else if (retSemaTy && retSemaTy->kind != sema::TypeRef::Kind::Unit &&
                   retTy != i32()) {
            // No annotation visible (e.g. an IIFE-invoked async lambda): fall
            // back to the body's natural result type as T, so `async () => 42`
            // still completes its task with 42. A void / Unit body leaves T null
            // (no result), avoiding lowering Unit to an unsized type.
            asyncResultElemSemaTy = retSemaTy;
        }
        retTy = ptrTy();  // the closure returns the outer Task pointer
        retSemaTy = nullptr;
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

    // §30.6 (#488): an `async` lambda's body is an async STATE MACHINE, not a
    // straight-line body. The closure entry fn (with its params + captured cells
    // + this now bound above) becomes the async ENTRY; emitAsyncLambda takes
    // over body emission (alloc the outer Task, frame, drive a Resume that holds
    // the suspend/resume body, return the Task) and terminates `fn`. Skip the
    // synchronous body-emission + fall-through-return below.
    //
    // CRITICAL: an async lambda is frequently nested inside an ENCLOSING async
    // method / lambda whose own state machine is mid-emission (lowerLambda was
    // called from the enclosing resume body). emitAsyncLambda overwrites and
    // then clears every sm*_/async*_ live-state field for the lambda's own
    // machine. Save and restore them ALL here so the enclosing machine's await
    // lowering (which resumes after the closure value is built) is unperturbed.
    // The non-async lambda body never touches these, so the save/restore is
    // scoped to the async branch.
    if (lam.isAsync) {
        SuspendKind savedSmKind = smKind_;
        llvm::StructType* savedSmStateStruct = smStateStruct_;
        llvm::Value* savedSmSelf = smSelf_;
        llvm::Value* savedSmStateGEP = smStateGEP_;
        auto savedSmStateBBs = smStateBBs_;
        llvm::BasicBlock* savedSmDoneBB = smDoneBB_;
        unsigned savedSmNextState = smNextSuspendState_;
        auto savedSmLocalFields = smLocalFields_;
        auto savedSmAllocaToField = smAllocaToField_;
        unsigned savedIterCurrentValField = iterCurrentValField_;
        llvm::Type* savedIterElemTy = iterElemTy_;
        unsigned savedAsyncOuterTaskField = asyncOuterTaskField_;
        unsigned savedAsyncPendingTaskField = asyncPendingTaskField_;
        unsigned savedAsyncSpillPtrField = asyncSpillPtrField_;
        auto savedAsyncHandlerSpillSlots = asyncHandlerSpillSlots_;
        llvm::Function* savedAsyncResumeFn = asyncResumeFn_;
        llvm::Type* savedAsyncResultTy = asyncResultTy_;
        const sema::TypeRef* savedAsyncResultSemaType = asyncResultSemaType_;
        // emitAsyncLambda recomputes closure-release eligibility for the LAMBDA's
        // own body and keys it to the lambda resume fn. The ENCLOSING function's
        // eligibility (which governs whether THIS `var f := <lambda>` local gets
        // its scope-end closure release — registered AFTER lowerLambda returns)
        // must be preserved; save and restore it across the call.
        auto savedClosureRelElig = closureReleaseEligible_;
        llvm::Function* savedClosureRelEligFn = closureReleaseEligibleFn_;

        emitAsyncLambda(lam, fn, paramSyms, asyncResultElemSemaTy);

        closureReleaseEligible_ = std::move(savedClosureRelElig);
        closureReleaseEligibleFn_ = savedClosureRelEligFn;
        smKind_ = savedSmKind;
        smStateStruct_ = savedSmStateStruct;
        smSelf_ = savedSmSelf;
        smStateGEP_ = savedSmStateGEP;
        smStateBBs_ = std::move(savedSmStateBBs);
        smDoneBB_ = savedSmDoneBB;
        smNextSuspendState_ = savedSmNextState;
        smLocalFields_ = std::move(savedSmLocalFields);
        smAllocaToField_ = std::move(savedSmAllocaToField);
        iterCurrentValField_ = savedIterCurrentValField;
        iterElemTy_ = savedIterElemTy;
        asyncOuterTaskField_ = savedAsyncOuterTaskField;
        asyncPendingTaskField_ = savedAsyncPendingTaskField;
        asyncSpillPtrField_ = savedAsyncSpillPtrField;
        asyncHandlerSpillSlots_ = std::move(savedAsyncHandlerSpillSlots);
        asyncResumeFn_ = savedAsyncResumeFn;
        asyncResultTy_ = savedAsyncResultTy;
        asyncResultSemaType_ = savedAsyncResultSemaType;
    } else
    // Emit body.
    if (lam.exprBody) {
        llvm::Value* v = lowerExpr(*lam.exprBody);
        if (!builder_.GetInsertBlock()->getTerminator()) {
            if (v && !retTy->isVoidTy()) {
                // fix #541 / §7.1: when the lambda's expected return type is a
                // NULLABLE VALUE TYPE (`T?` → `{i8 hasValue; T value}`) but the
                // body expression lowered to the bare underlying `T` (or a
                // narrower numeric that sema widened — e.g. `int` literal in
                // `double?` context), promote the value exactly as
                // `var x: T? := <nonnull>` does — using wrapNullableValue
                // (hasValue=1).  The `null` pointer literal lowers to a
                // ConstantPointerNull (ptr-typed); map that to the all-zero
                // tagged-null struct instead.  Without this promotion the `ret`
                // instruction carries a bare (possibly mistyped) value into a
                // `{i8,T}`-returning function, which the LLVM verifier rejects.
                // per §7.1 / §7.2.
                if (retSemaTy && isNullableValueTy(retSemaTy) &&
                    v->getType() != retTy) {
                    auto* expectedSt = nullableValueStructTy(retSemaTy);
                    if (v->getType()->isPointerTy() &&
                        llvm::isa<llvm::ConstantPointerNull>(v)) {
                        v = nullableValueNull(retSemaTy);
                    } else {
                        llvm::Type* payloadTy = expectedSt->getElementType(1);
                        // Apply numeric widening (§5.1: int→double etc.) before
                        // wrapping, mirroring coerceNumericToType in
                        // lowerLocalDecl.  This handles `() => 3` in double?
                        // context where the literal is i32 but the payload is f64.
                        if (v->getType() != payloadTy)
                            v = coerceNumericToType(v, payloadTy);
                        if (v->getType() == payloadTy)
                            v = wrapNullableValue(retSemaTy, v);
                    }
                }
                builder_.CreateRet(v);
            } else {
                builder_.CreateRetVoid();
            }
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
    finallyStack_ = std::move(savedFinallyStack);  // #546 / §31.2: restore
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
    const sema::TypeSymbol* t = nullptr;
    if (rt && rt->kind == sema::TypeRef::Kind::Named && rt->declaration) {
        t = rt->declaration;
    } else {
        // #575 / §9.5 / §22.3: `this` inside a record instance method has no
        // sema exprType (the member-access resolver uses currentType_ directly,
        // so ExprTypecheck leaves `this` unannotated for named-type methods).
        // When the source is the receiver pointer (src == currentThis_), recover
        // the owner TypeSymbol by reverse-looking currentFn_ in methodFns_ so
        // the record `with`-copy path can determine the struct layout and field
        // indices. Without this, lowerWith returns the raw pointer and the
        // method emits `ret ptr` where `ret %record.T` is expected →
        // IR verification failure.
        // fix #628 / §14.2: when emitting inside buildSpecializedMethodFn,
        // currentFn_ is the freshly-created specFn which is NOT registered in
        // methodFns_ (only the template function is).  Fall back to
        // currentSpecMethodSymbol_ (the original MethodSymbol* being specialized)
        // to recover the parent record TypeSymbol.
        if (src == currentThis_ && currentFn_) {
            for (const auto& [ms, fn] : methodFns_) {
                if (fn == currentFn_ && ms->parent &&
                    ms->parent->kind == ast::TypeDeclKind::Record) {
                    t = ms->parent;
                    rt = nullptr;  // no concrete TypeRef; use method's own subst
                    break;
                }
            }
            if (!t && currentSpecMethodSymbol_ &&
                currentSpecMethodSymbol_->parent &&
                currentSpecMethodSymbol_->parent->kind == ast::TypeDeclKind::Record) {
                t = currentSpecMethodSymbol_->parent;
                rt = nullptr;  // genericSubst_ already active from buildSpecializedMethodFn
            }
        }
        if (!t) return src;
    }
    const bool isRecord = t->kind == ast::TypeDeclKind::Record;
    // issue #462 / §9.5 / §14.1: a GENERIC record `with`-copy must use the
    // SPECIALIZED struct layout for the source's instantiation, not the i32
    // placeholder. With the placeholder the spilled copy (and each replacement
    // store) is sized/offset wrong — an 8-byte reference field (string / class
    // ptr) is truncated to 4 bytes, corrupting the pointer (SIGSEGV on the next
    // dereference); an 8-byte value field (long / double / 2-int record) silently
    // loses its upper 4 bytes. RecordSubstGuard installs the source TypeRef's
    // concrete type args so classStruct → recordStruct produces the right layout;
    // the substitution stays active through the per-field replacement stores
    // (which use fieldIndex_ as registered by recordStruct) and the final reload.
    // A no-op for a non-generic record or a class source.
    RecordSubstGuard withSubst(this, rt);
    // classStruct routes records to recordStruct (no vtable), so this is
    // the right layout for both forms.
    llvm::StructType* st = classStruct(t);
    // The GEP base: a spilled copy for a record, the instance pointer for
    // a class.
    llvm::Value* base = src;
    if (isRecord) {
        base = entryAlloca(st);
        // #575 / §9.5 / §22.3: when `this with { … }` is used inside a record
        // instance method, lowerExpr(this) returns currentThis_ which is a
        // POINTER to the record (CG29 — record methods receive `this` as ptr,
        // §9.4). A regular non-this record source is already a struct VALUE.
        // Load the struct from the pointer before spilling to the copy alloca;
        // storing a ptr into a struct-typed alloca would corrupt the copy and
        // produce a "return type does not match" IR verification failure.
        llvm::Value* srcVal = src->getType()->isPointerTy()
                              ? builder_.CreateLoad(st, src, "this.val")
                              : src;
        builder_.CreateStore(srcVal, base);
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
        llvm::Value* slot = builder_.CreateStructGEP(st, base, idxIt->second);
        // #429 / §4.6 / §28.9 / §22.11: a CLASS object-initializer store into a
        // string field/auto-property takes an independent +1 (released on the
        // instance's disposal), exactly like a plain assignment — retain-new (or
        // transfer a producer) / release-old. The freshly-`new`'d field already
        // holds its default-initializer value (retained by emitInstanceInit) or
        // null, so release the old after storing the new. A RECORD `with`-copy is
        // a value type whose string fields are borrows (§9) and has no disposer —
        // it stays a plain store (no retain).
        if (!isRecord && v->getType()->isPointerTy() && isStringStorageMember(m)) {
            llvm::Value* oldVal = builder_.CreateLoad(ptrTy(), slot, "strf.old");
            if (!isPlusOneStringProducer(a.value.get()))
                emitStringRetain(v);
            builder_.CreateStore(v, slot);
            emitStringRelease(oldVal);
            continue;
        }
        builder_.CreateStore(v, slot);
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
            // per §9 (bug D): a record element type lowers to an LLVM
            // StructType whose allocation size may exceed 8 bytes — use the
            // DataLayout for structs so bz_array_alloc sizes the backing
            // buffer correctly (same fix as collectionElemSize in Calls.cpp).
            int64_t elemSize = 8;  // default: pointer
            if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                llvm::Type* et = lowerTypeRef(rt->genericArgs[0]);
                if (et) {
                    if (et->isIntegerTy(32) || et->isFloatTy()) elemSize = 4;
                    else if (et->isIntegerTy(1) || et->isIntegerTy(8)) elemSize = 1;
                    else if (et->isIntegerTy(16)) elemSize = 2;
                    else if (et->isStructTy())
                        elemSize = (int64_t)module_->getDataLayout().getTypeAllocSize(et);
                    else elemSize = 8;  // ptr, i64, double
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
                // StringBuilder is a bz_array_t of string *pointers* (parts list).
                // Each element is a pointer-sized slot (8 bytes), matching SB_ESZ=8
                // used by every Append/Insert/Remove/Replace/Length/ToString operation
                // in LlvmBackend_Calls.cpp. Fix for #497: the old elem_size=2 produced
                // a 128-byte backing buffer (64*2), but each Append advanced 8 bytes,
                // overflowing after ~16 appends and corrupting allocator metadata.
                // per §6 / §3.1.
                constexpr int64_t SB_ESZ = 8;
                auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                llvm::Value* sb = builder_.CreateCall(allocFn,
                    {builder_.getInt64(SB_ESZ), builder_.getInt64(64)});
                // Fix #531 (§3.1): new StringBuilder(initial: string) — if a string
                // arg is present, append it as the first part via bz_array_add,
                // reusing the same IR sequence as Append(string) in Calls.cpp.
                // The no-arg ctor and capacity-int ctor are not affected: the
                // capacity arg has an int type (not String), so isStrArg is false.
                if (!ne.args.empty() && ne.args[0].value) {
                    auto* argTy = ctx_.exprType(*ne.args[0].value);
                    bool isStrArg = argTy &&
                        argTy->kind == sema::TypeRef::Kind::Primitive &&
                        argTy->primitive == sema::PrimitiveType::String;
                    if (isStrArg) {
                        llvm::Value* s = lowerExpr(*ne.args[0].value);
                        if (s) {
                            llvm::AllocaInst* slot = entryAlloca(ptrTy());
                            builder_.CreateStore(s, slot);
                            auto* addFn = declareC("bz_array_add",
                                                   llvm::Type::getVoidTy(llvm_),
                                                   {ptrTy(), ptrTy(), i64()});
                            builder_.CreateCall(addFn,
                                {sb, slot, builder_.getInt64(SB_ESZ)});
                        }
                    }
                }
                return sb;
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
            // §28.13 / stdlib §2.16 (#603): new Shared<T>(owned_value) —
            // allocate a control block and store the owned object pointer.
            // bz_shared_new(value: ptr, dispose_value: fn) → ptr (control block).
            // The dispose_value callback is the class element disposer thunk
            // (vtable Dispose + free), so the inner T is disposed exactly once
            // when the last strong handle is released (§28.13).
            if (colName == "Shared" && ne.args.size() == 1 && ne.args[0].value) {
                llvm::Value* val = lowerExpr(*ne.args[0].value);
                if (!val) return nullptr;
                // Get the element disposer thunk for T (the inner class type).
                // #602 batch 4 / §28.11 v0.135 / §28.13: Shared<T>'s inner T is
                // always a class type (sema-enforced); ClassOwned row → vtable-
                // dispatch disposer thunk. elementDisposerThunk() is the shared
                // void(void*) shim (vtable slot 2 Dispose + free); it is also
                // what elementPolicyFor returns for ClassOwned, so using it here
                // is policy-consistent (same thunk used for OwnedArray<T> disposal).
                llvm::Function* dispFn = elementDisposerThunk();
                auto* newFn = declareC("bz_shared_new", ptrTy(), {ptrTy(), ptrTy()});
                return builder_.CreateCall(newFn, {val, dispFn}, "shared.new");
            }
            // §30.15 (#600 phase c): new Channel<T>() — allocate a bz_channel_t.
            // The dispose_elem callback is derived from elementPolicyFor:
            //   - ClassOwned → elementPolicyFor().disposerThunk (covers class,
            //     interface, Shared<T>, Weak<T> — per §28.11 v0.135)
            //   - value/string/RuntimeManaged → null (no deep disposal needed)
            // #602 batch 4: routed through elementDisposerArg (the #545 hook) so
            // the null-vs-thunk decision is consistent with every other site.
            if (colName == "Channel") {
                const sema::TypeRef* chElemTy =
                    (!rt->genericArgs.empty()) ? rt->genericArgs[0] : nullptr;
                // Channel<T> behaves like an owning collection for class elements
                // (per §30.15 / §28.11 v0.135: class elements are owned, the
                // runtime disposes them when the channel is closed/drained).
                llvm::Value* dispArg = elementDisposerArg(chElemTy, /*owningFamily=*/true);
                auto* allocFn = declareC("bz_channel_new", ptrTy(), {ptrTy()});
                return builder_.CreateCall(allocFn, {dispArg}, "ch.new");
            }
            // §6.1 / §30.8: new CancellationTokenSource() — allocate via
            // bz_cts_alloc (stdlib §6.1 default constructor).
            if (colName == "CancellationTokenSource") {
                auto* allocFn = declareC("bz_cts_alloc", ptrTy(), {});
                return builder_.CreateCall(allocFn, {}, "cts.alloc");
            }
            // per §6.8 / #652: new Domain(name: string) — allocate a named
            // domain instance backed by a bz_domain_t with a stored name field.
            // bz_domain_alloc(name_str_p) → bz_domain_t* (refs=1 for `using var`).
            if (colName == "Domain") {
                llvm::Value* nameArg = llvm::ConstantPointerNull::get(ptrTy());
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* v = lowerExpr(*ne.args[0].value);
                    if (v) nameArg = v;
                }
                auto* allocFn = declareC("bz_domain_alloc", ptrTy(), {ptrTy()});
                return builder_.CreateCall(allocFn, {nameArg}, "dom.alloc");
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

            // §2.15 (issue #550): BinarySerializer / BinaryDeserializer.
            // These require `import System.Serialization`; they are compiler-known
            // builtin classes backed by bz_binser_t / bz_binder_t in the runtime.
            // new BinarySerializer() → bz_binser_alloc()  (opaque ptr)
            if (colName == "BinarySerializer" && ne.args.empty()) {
                auto* fn = declareC("bz_binser_alloc", ptrTy(), {});
                return builder_.CreateCall(fn, {}, "binser.new");
            }
            // new BinaryDeserializer(span: ReadOnlySpan<byte>)  or
            // new BinaryDeserializer(span, ignoreUnknownFields: bool)
            // The span arg is an { ptr, i32, i32 } struct value; we extract
            // the backing data pointer and byte count.
            if (colName == "BinaryDeserializer" && !ne.args.empty()) {
                if (!ne.args[0].value) return nullptr;
                llvm::Value* spanVal = lowerExpr(*ne.args[0].value);
                if (!spanVal) return nullptr;
                // Extract source ptr (field 0) and length i32 (field 2) from the span.
                llvm::Value* dataPtr = spanSource(spanVal);  // ptr to raw bytes
                llvm::Value* lenI32  = spanLength(spanVal);  // i32 element count
                // The data ptr from the span points to the bz_array_t->data buffer;
                // for byte spans (elem_size=1), that IS the raw byte data.
                // However, the source field of a byte span IS the bz_array_t* header,
                // not data directly. We need the actual byte data, so GEP into the
                // array's data field. Use bz_owned_buffer_get_span to unpack.
                // Actually: spanSource returns the bz_array_t* (the "source" ptr in
                // the span struct). We need to get arr->data from it.
                // bz_array_t layout: {void* data, i64 count, i64 capacity, i8 disposed}
                // data is at offset 0.
                llvm::Value* arrData = builder_.CreateLoad(ptrTy(), dataPtr,
                    "bindeser.data");
                // Apply the span start offset (usually 0, but can be non-zero).
                llvm::Value* startI32 = spanStart(spanVal);
                llvm::Value* startI64 = builder_.CreateZExt(startI32, i64(), "span.s64");
                llvm::Value* lenI64   = builder_.CreateZExt(lenI32, i64(), "span.l64");
                // Offset = start (in bytes, elem_size=1 for byte span)
                llvm::Value* byteData = builder_.CreateGEP(
                    llvm::Type::getInt8Ty(llvm_), arrData, startI64, "bindeser.bdata");
                // ignoreUnknownFields: second arg if present (bool, default 0)
                llvm::Value* ignoreFld = builder_.getInt8(0);
                if (ne.args.size() >= 2 && ne.args[1].value) {
                    llvm::Value* bv = lowerExpr(*ne.args[1].value);
                    if (bv)
                        ignoreFld = builder_.CreateZExt(bv, i8(), "deser.ign");
                }
                auto* fn = declareC("bz_binder_alloc", ptrTy(),
                                    {ptrTy(), i64(), i8()});
                return builder_.CreateCall(fn, {byteData, lenI64, ignoreFld},
                                           "bindeser.new");
            }
        }
        return nullptr;
    }
    const sema::TypeSymbol* t = rt->declaration;

    // CG-generic: if this is a generic instantiation (e.g. Box<int>),
    // set up the type-parameter substitution before classStruct / recordStruct
    // so that field types are computed with the concrete type arguments.
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

    // §9 / §9.5 / issue #460: `new Record(args)` builds a value-type struct
    // directly — no heap allocation, vtable, constructor chain, or disposal.
    // For a GENERIC record (`new Box<string>(...)`) the substitution set up
    // above MUST be active across buildRecordValue so recordStruct lays the
    // T-typed fields out as the concrete specialized types (the same way the
    // class path below carries the subst through classStruct + the inline
    // constructor). Restore the outer substitution afterward.
    if (t->kind == ast::TypeDeclKind::Record) {
        // issue #597: pass the OUTER substitution (savedSubst/savedSemaTy) so
        // construction-argument expressions are lowered under the enclosing
        // scope's binding while the record's field layout uses the new type's
        // substitution (active in genericSubst_).
        llvm::Value* rec = buildRecordValue(t, ne.args, savedSubst, savedSemaTy);
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        return rec;
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

    // §14.1 / §2.5 / §2.6 (issue #328 FINDING [2]; #428): an IMPORTED GENERIC
    // type has NO stable per-type symbols to link against — a generic is
    // monomorphized per-instantiation (§14), so the dependency emits no
    // `bz$<Type>$init` (the declareFunctions carve-out exports init only for
    // non-generic public types) and its `bz$<Type>$ctor` is the template (T→i32),
    // not this instantiation. Sema rejects `new ImportedGeneric<...>(...)` before
    // codegen runs (checkImportedNewExprArgs for a class, the NewExpr record guard
    // for a record), so this branch is normally unreachable for a generic imported
    // type; the guard is a BACKSTOP so codegen never emits a dangling external
    // `bz$<Type>$init`/`$ctor` reference (the FINDING [2] link failure) even if
    // reached out-of-band. The bare calloc'd object is returned as-is. Sound
    // cross-module generic construction needs importer-side per-instantiation
    // infrastructure (a vtable + disposer for a generic class; record-struct
    // specialization for a generic record) — a larger feature, deferred (#428).
    const bool importedGeneric =
        ctx_.isImported(t) && !t->genericParams.empty();
    if (ctx_.isImported(t) && !importedGeneric) {
        llvm::Function* initFn = declareImportedInit(t);
        builder_.CreateCall(initFn, {obj});
    } else if (!ctx_.isImported(t)) {
        // §22.10 / §22.8 (#478): a class with primary-constructor parameters
        // must have those parameter values stored into their field slots
        // BEFORE field initializers run (§22.8 step 0), so that initializers
        // like `property X := x` can load the already-stored value via
        // implicitFieldAddr. Also set currentThis_ = obj for the duration of
        // emitInstanceInit so that implicitFieldAddr resolves against the new
        // object, not the surrounding method's `this`.
        llvm::Value* savedThisForInit = currentThis_;
        currentThis_ = obj;
        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (td && td->kind == ast::TypeDeclKind::Class &&
            td->hasPositionalFields && !td->positionalFields.empty()) {
            // Only the all-positional case: store each arg value directly into
            // the corresponding primary-ctor-param field slot.
            bool allPositional = true;
            for (const auto& a : ne.args)
                if (!a.name.empty()) { allPositional = false; break; }
            if (allPositional &&
                ne.args.size() == td->positionalFields.size()) {
                for (std::size_t pi = 0; pi < td->positionalFields.size(); ++pi) {
                    const ast::Param& pf = td->positionalFields[pi];
                    for (sema::Symbol* m : t->members) {
                        auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
                        if (!fs || fs->posParam != &pf) continue;
                        auto idxIt = fieldIndex_.find(fs);
                        if (idxIt == fieldIndex_.end()) break;
                        if (!ne.args[pi].value) break;
                        // issue #597: a positional primary-ctor argument is an
                        // enclosing-scope expression — lower it under the outer
                        // substitution, then restore the new type's subst for
                        // the field-type coercion + store below.
                        auto posSubst = genericSubst_;
                        auto posSemaTy = genericSubstSemaTy_;
                        genericSubst_ = savedSubst;
                        genericSubstSemaTy_ = savedSemaTy;
                        llvm::Value* v = lowerExpr(*ne.args[pi].value);
                        genericSubst_ = std::move(posSubst);
                        genericSubstSemaTy_ = std::move(posSemaTy);
                        if (!v) break;
                        llvm::Type* fieldTy =
                            st->getElementType(idxIt->second);
                        v = coerceNumericToType(
                            v, fieldTy,
                            isUnsignedTargetExpr(*ne.args[pi].value));
                        builder_.CreateStore(
                            v, builder_.CreateStructGEP(
                                   st, obj, idxIt->second));
                        break;
                    }
                }
            }
        }
        emitInstanceInit(t, obj, st);
        currentThis_ = savedThisForInit;
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
            // issue #597 (§14.1 / §22.4): the constructor ARGUMENT expressions
            // belong to the ENCLOSING scope, so they must be lowered under the
            // outer substitution (`savedSubst`), not the new type's own
            // substitution that was activated above for the constructor body /
            // field layout.  Inside `Pair<int,string>.Swap()` the body is
            // `new Pair<B,A>(_b, _a)`: the new type's subst is `{A→string,
            // B→int}` (the swapped result), but `_b`/`_a` read `this`, whose
            // layout is the receiver's `{A→int, B→string}`.  Lowering the args
            // under the new subst laid `this` out as `Pair<string,int>` and
            // read the fields at the wrong offsets/widths (returned garbage).
            // Lower the arg values under the outer subst, then restore the new
            // subst so the constructor body / field stores keep the right
            // (specialized) layout.
            auto ctorArgSubst = genericSubst_;
            auto ctorArgSemaTy = genericSubstSemaTy_;
            genericSubst_ = savedSubst;
            genericSubstSemaTy_ = savedSemaTy;
            bool builtArgs = buildOrderedArgs(cd->params, ne.args, args);
            genericSubst_ = std::move(ctorArgSubst);
            genericSubstSemaTy_ = std::move(ctorArgSemaTy);
            if (builtArgs &&
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
                    // unwind chain — not a dedicated ctor cleanup block.
                    // inlineCtorBody builds that cleanup block (assigned owned
                    // fields + base subobject + free, continuing to the
                    // ENCLOSING unwind), runs the leading `base(...)`/`this(...)`
                    // chain (§12 — recursively inlining a generic base ctor so
                    // its field stores use the specialized layout, issue #585),
                    // then lowers the remaining body. Without running the chain,
                    // a generic base's inherited fields stayed calloc-zeroed
                    // (the base ctor never ran), so a `protected` base field read
                    // 0 in/through the derived class (#585).
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
                    inlineCtorBody(cs, obj);
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
    // §5.2 (L421): casting an integer to char requires the value to be in
    // U+0000–U+10FFFF excluding the surrogate range U+D800–U+DFFF; otherwise
    // raise OverflowException.  This check must precede the same-width-identity
    // path below (int → char: both are i32, so srcTy==dstTy would skip it).
    if (target == P::Char && src->getType()->isIntegerTy()) {
        // Widen to i64 for the unsigned range check so negative int values
        // (sign-extended) compare correctly against the unsigned bounds.
        llvm::Type* i64ty = llvm::Type::getInt64Ty(llvm_);
        llvm::Value* v64  = builder_.CreateSExt(src, i64ty, "ctoi64");
        // below zero?
        llvm::Value* neg  = builder_.CreateICmpSLT(
            v64, llvm::ConstantInt::get(i64ty, 0), "cneg");
        // above U+10FFFF?
        llvm::Value* hi   = builder_.CreateICmpSGT(
            v64, llvm::ConstantInt::get(i64ty, 0x10FFFF), "chi");
        // in surrogate range [0xD800, 0xDFFF]?
        llvm::Value* sg_lo = builder_.CreateICmpSGE(
            v64, llvm::ConstantInt::get(i64ty, 0xD800), "csglo");
        llvm::Value* sg_hi = builder_.CreateICmpSLE(
            v64, llvm::ConstantInt::get(i64ty, 0xDFFF), "csghi");
        llvm::Value* surr  = builder_.CreateAnd(sg_lo, sg_hi, "csurr");
        // any of the three conditions triggers OverflowException
        llvm::Value* bad   = builder_.CreateOr(
            builder_.CreateOr(neg, hi, "coor"), surr, "cbad");
        emitGuardThrow(bad, "OverflowException");
        // Value is now known valid; truncate or keep as i32 char.
        llvm::Type* dstCharTy = lowerPrimitive(P::Char);  // i32
        if (src->getType() != dstCharTy)
            return builder_.CreateTrunc(src, dstCharTy);
        return src;
    }
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


llvm::Value* Emitter::buildRecordValue(
        const sema::TypeSymbol* t,
        const std::vector<ast::CallArgument>& args,
        const std::unordered_map<std::string_view, llvm::Type*>& argSubst,
        const std::unordered_map<std::string_view, const sema::TypeRef*>&
            argSemaTy) {
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
    //
    // §22.5 / #540: named arguments on a positional record are also handled
    // here by reordering the supplied args to field-declaration order before
    // the field stores, mirroring buildOrderedArgs for class constructors.
    if (!hasAnyConstructor(t) && !args.empty() &&
        args.size() == fields.size()) {
        bool allPositional = true;
        for (const auto& a : args)
            if (!a.name.empty()) { allPositional = false; break; }

        // Build a field-index-ordered pointer to each call argument.
        // For positional args: fieldArg[i] = &args[i].
        // For named args: fieldArg[i] = the arg whose .name == fields[i]->name.
        // Fall through to the body-record path if any name is unrecognised.
        std::vector<const ast::CallArgument*> fieldArg(fields.size(), nullptr);
        bool mappingOk = true;
        if (allPositional) {
            for (std::size_t i = 0; i < fields.size(); ++i)
                fieldArg[i] = &args[i];
        } else {
            // §22.5 / #540: map each named argument to the field with that name.
            // per §22.5, named arguments may appear in any order after positional
            // arguments, but a positional record has no explicit constructor so
            // all arguments must be named (or all positional — the mixed case
            // is rejected by sema).
            for (std::size_t ai = 0; ai < args.size(); ++ai) {
                const ast::CallArgument& a = args[ai];
                if (a.name.empty()) {
                    // positional arg in a mixed call — fall through to body path.
                    mappingOk = false; break;
                }
                bool found = false;
                for (std::size_t fi = 0; fi < fields.size(); ++fi) {
                    if (fields[fi]->name == a.name) {
                        fieldArg[fi] = &a;
                        found = true;
                        break;
                    }
                }
                if (!found) { mappingOk = false; break; }
            }
            // Verify every field got an argument.
            if (mappingOk) {
                for (std::size_t fi = 0; fi < fields.size(); ++fi)
                    if (!fieldArg[fi]) { mappingOk = false; break; }
            }
        }

        if (mappingOk) {
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (!fieldArg[i] || !fieldArg[i]->value) return nullptr;
                // issue #597: lower the construction argument under the
                // ENCLOSING substitution (its expression belongs to the outer
                // scope), then restore the record's own substitution for the
                // field-index store below.
                auto recSubst = genericSubst_;
                auto recSemaTy = genericSubstSemaTy_;
                genericSubst_ = argSubst;
                genericSubstSemaTy_ = argSemaTy;
                llvm::Value* v = lowerExpr(*fieldArg[i]->value);
                genericSubst_ = std::move(recSubst);
                genericSubstSemaTy_ = std::move(recSemaTy);
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
        // issue #597: lower the constructor arguments under the enclosing
        // substitution (see the positional path above), then restore the
        // record's own substitution for the inlined constructor body.
        auto recSubst = genericSubst_;
        auto recSemaTy = genericSubstSemaTy_;
        genericSubst_ = argSubst;
        genericSubstSemaTy_ = argSemaTy;
        bool builtRecArgs = buildOrderedArgs(cd->params, args, ordered);
        genericSubst_ = std::move(recSubst);
        genericSubstSemaTy_ = std::move(recSemaTy);
        if (!builtRecArgs) return nullptr;
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
    const sema::TypeRef* resultSemaTy = ctx_.exprType(ce);
    llvm::Type* resultTy = i32();
    if (resultSemaTy) resultTy = lowerTypeRef(resultSemaTy);

    auto* endBB = llvm::BasicBlock::Create(llvm_, "case.end", currentFn_);
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incomings;

    for (const auto& arm : ce.arms) {
        if (arm.bodyKind != ast::CaseArmBodyKind::Expression ||
            !arm.exprBody)
            return nullptr;  // block-body arms deferred
        llvm::Value* pm = patternMatches(arm.pattern.get(), scrut, enumTy,
                                         isStr, scrutTy);
        // #473: with type-test (`Dog`), primitive-type (`int`), typed-binding
        // (`var x: Dog`/`var v: int`), and the existing constant/range/null/
        // destructure patterns all lowered, the #473 repros no longer reach
        // here. A null result now means a genuinely unsupported pattern shape
        // (e.g. a bare record name `Pt` with no parens). Returning null bails
        // out of lowerCaseExpr; the half-built `case.end` is left
        // unterminated, which the always-on IR verifier reports as a
        // COMPILE-TIME error — the correct, visible failure for a not-yet-
        // implemented pattern (better than emitting a binary that traps at
        // run time). The #473 fix is in patternMatches above, not here.
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
            // fix #573 / §19.2 / §7.1 / §7.5: defer the branch-to-endBB so
            // nullable widening can emit into this block after all arms are
            // seen (same discipline as lowerConditional — see Path A/B there).
            // The CreateBr(endBB) terminators are emitted in the post-loop
            // widening pass below.
            incomings.push_back({v, builder_.GetInsertBlock()});
            // Do NOT create br here — deferred to post-loop.
        }
        // Reset for the next arm's pattern-test / body: a prior arm's
        // divergence must not bleed into the next arm.
        diverged_ = false;
        builder_.SetInsertPoint(nextBB);
    }
    // No arm matched → MatchFailureException (§19.6).
    // (The current insert point is the last arm's nextBB, which is exactly
    // where control falls when no arm matched — correct placement for the
    // MatchFailure throw.)
    emitThrowBuiltin("MatchFailureException");

    // fix #573 / §19.2 / §7.1 / §7.5: nullable VALUE-TYPE widening for the
    // case result PHI.  A `case n: int  1 => 100, _ => null end` whose
    // declared return type is `int?` produces arm values of different LLVM
    // types: `100` is a bare i32 while `null` is a ConstantPointerNull (ptr)
    // — sema resolves the common result type as the nullable struct but
    // lowerExpr for an integer literal emits a bare scalar, and lowerExpr for
    // the `null` literal uses ptr null when the sema type is a reference type.
    // The same mismatch occurs even when sema resolves to a non-nullable
    // reference type (ptr) but one arm emitted a value struct.
    //
    // Apply exactly the same two paths as lowerConditional (§17 / §7):
    //
    //   Path A — sema result IS a nullable VALUE type: widen each arm to the
    //   tagged {i8 hasValue; T value} struct using the existing helpers.
    //   • bare T               → wrapNullableValue (hasValue=1, T)
    //   • ConstantPointerNull  → nullableValueNull  (hasValue=0, T=0)
    //   • already the struct   → pass through
    //
    //   Path B — sema result is a REFERENCE type (ptr) but arm values are
    //   mismatched (some ptr-null, some non-ptr): derive the nullable struct
    //   from the non-null arm's LLVM type and widen all arms.  This handles
    //   `1 => 100, _ => null` whose sema result is Object? (ptr) but one arm
    //   lowered to i32.
    //
    // In both paths, widening instructions for non-null arms are emitted
    // INSIDE THAT ARM'S BLOCK (the block recorded in incomings[i].second,
    // set as insert point before each widen so the result dominates the PHI
    // edge). Branch terminators (CreateBr(endBB)) are emitted per arm after
    // widening, also inside the arm's block, before moving to endBB.
    if (!incomings.empty()) {
        bool needPathA = resultSemaTy && isNullableValueTy(resultSemaTy);
        if (needPathA) {
            // Path A: sema-guided widening to the nullable struct.
            llvm::StructType* nst = nullableValueStructTy(resultSemaTy);
            auto widenOne = [&](llvm::Value*& v) {
                if (!v || v->getType() == nst) return;
                if (v->getType()->isPointerTy() &&
                    llvm::isa<llvm::ConstantPointerNull>(v)) {
                    v = nullableValueNull(resultSemaTy);  // constant, no instrs
                } else if (v->getType() == nst->getElementType(1)) {
                    v = wrapNullableValue(resultSemaTy, v);  // emits into bb
                }
            };
            for (auto& [v, bb] : incomings) {
                builder_.SetInsertPoint(bb);
                widenOne(v);
            }
        } else {
            // Path B: no sema-supplied nullable struct; detect ptr-null vs
            // non-ptr mismatch across all arms and reconcile using the
            // non-null arm's LLVM type.
            auto isNullPtr = [](llvm::Value* v) {
                return v->getType()->isPointerTy() &&
                       llvm::isa<llvm::ConstantPointerNull>(v);
            };
            // Check whether there is a mixed-type situation: at least one
            // ptr-null arm AND at least one non-ptr arm.
            bool hasNullPtr = false, hasNonPtr = false;
            llvm::Value* nonPtrSample = nullptr;
            for (auto& [v, bb] : incomings) {
                if (isNullPtr(v)) { hasNullPtr = true; }
                else              { hasNonPtr = true; nonPtrSample = v; }
            }
            if (hasNullPtr && hasNonPtr && nonPtrSample) {
                // Derive the nullable struct type from the non-ptr arm.
                llvm::Type* innerTy = nonPtrSample->getType();
                llvm::StructType* nst;
                if (auto* st = llvm::dyn_cast<llvm::StructType>(innerTy);
                    st && st->getNumElements() == 2 &&
                    st->getElementType(0) == nullablePrimHasValTy()) {
                    // Non-ptr arm already has the {i8, T} struct — use as-is.
                    nst = st;
                } else {
                    // Non-ptr arm is a bare T; build the {i8, T} struct type.
                    nst = llvm::StructType::get(
                        llvm_, {nullablePrimHasValTy(), innerTy});
                }
                for (auto& [v, bb] : incomings) {
                    if (isNullPtr(v)) {
                        // null → all-zero constant, no instructions needed.
                        v = llvm::ConstantAggregateZero::get(nst);
                    } else if (v->getType() != nst) {
                        // bare T → wrap into {i8 1, T} in the arm's block.
                        builder_.SetInsertPoint(bb);
                        llvm::Value* res = builder_.CreateInsertValue(
                            llvm::UndefValue::get(nst),
                            llvm::ConstantInt::get(nullablePrimHasValTy(), 1),
                            {0u});
                        v = builder_.CreateInsertValue(res, v, {1u});
                    }
                    // else: already nst — pass through.
                }
            }
        }
    }

    // Emit branch terminators to endBB for each non-diverged arm.  This is
    // deferred until after widening so the widening instructions precede the
    // branch in the arm's block (required for dominance / correct IR).
    for (auto& [v, bb] : incomings) {
        builder_.SetInsertPoint(bb);
        builder_.CreateBr(endBB);
    }

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
    // test equality.
    //
    // #473 / §19.3 + §19.5: a bare type name `Dog` is *also* a NamedPattern
    // (the parser emits NamedPattern for any non-primitive identifier and
    // lets sema/codegen disambiguate Constant-vs-Type-test per §A.7). When
    // the name resolves to a class/interface type, the pattern is a
    // **type-test pattern**: it matches when the scrutinee is-a that type
    // ("Subtypes match: a Dog value matches an Animal pattern", §19.3) and,
    // per §19.5, only when the scrutinee is non-null — emitIsA returns false
    // for a null object pointer (its type-tag walk dereferences the vtable
    // only on a non-null pointer), so the is-a test embeds the non-null
    // requirement for reference scrutinees. Without this handler `Dog =>`
    // returned nullptr → lowerCaseExpr bailed leaving `case.end`
    // unterminated (the #473 malformed-IR / crash).
    if (auto* np = dynamic_cast<const ast::NamedPattern*>(pat)) {
        if (enumTy && np->name && !np->name->parts.empty()) {
            if (llvm::Value* cv =
                    enumConstantByName(enumTy, np->name->parts.back()))
                return emitEquals(scrut, cv, isStr);
        }
        // §19.3/§19.5: type-test pattern on a class/interface scrutinee.
        if (llvm::GlobalVariable* desc =
                descriptorForPatternName(np->name.get()))
            return emitIsA(scrut, desc);
        return nullptr;  // unresolved name — deferred
    }
    // #473 / §19.3: a primitive type pattern `int`, `string`, … (TypePattern).
    // The parser routes primitive-type-keyword patterns here (Parser_Exprs
    // parsePattern). When the scrutinee's static type already IS that
    // primitive (the well-typed case — sema requires the pattern type be
    // compatible with the scrutinee), the value is present and the type test
    // is statically satisfied: a non-nullable primitive is never null, so
    // §19.5's "matches only non-null values" holds trivially. (A nullable
    // VALUE-type scrutinee is handled by the isNullableVal branch above,
    // which never falls through to here.)
    if (dynamic_cast<const ast::TypePattern*>(pat))
        return builder_.getInt1(true);
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
            // §19.3/§19.5: `var x: T` is a type-test + binding — it matches
            // if the scrutinee is-a T and (§19.5) is non-null.
            if (auto* rt = ctx_.resolvedType(*bp->type)) {
                if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration) {
                    auto dit = descArray_.find(rt->declaration);
                    if (dit != descArray_.end())
                        return emitIsA(scrut, dit->second);
                    // #473: a named class with no descArray_ entry can still
                    // be resolved by name (mirrors the bare-NamedPattern path).
                    if (llvm::GlobalVariable* desc =
                            descriptorForPatternName(bp->type->name.get()))
                        return emitIsA(scrut, desc);
                }
                // #473 / §19.3: `var v: P` where P is a primitive type. When
                // the scrutinee's static type already IS that primitive (the
                // well-typed case), the value is present and the type test is
                // statically satisfied — a non-nullable primitive is never
                // null (§19.5 holds trivially). The nullable VALUE-type
                // scrutinee path is handled by the isNullableVal branch above
                // and never reaches here.
                if (rt->kind == sema::TypeRef::Kind::Primitive)
                    return builder_.getInt1(true);
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
                // Bare `Pt(x, y)` binding: store field value into a slot
                // keyed on the PositionalPattern address — mirrors the sema
                // anchor (`lsym->decl = reinterpret_cast<...>(&sp)`).
                llvm::AllocaInst* slot = entryAlloca(fieldVal->getType());
                builder_.CreateStore(fieldVal, slot);
                const auto* key = reinterpret_cast<const ast::Node*>(&sp);
                locals_[key] = slot;
            } else if (auto* vbp = dynamic_cast<const ast::BindingPattern*>(
                           sp.pattern.get())) {
                // §19.3: `Pt(var x, var y)` — equivalent to bare form.
                // Sema sets lsym->decl = vbp (the BindingPattern node), so
                // key the slot on vbp to match localSlot()'s lookup.
                llvm::AllocaInst* slot = entryAlloca(fieldVal->getType());
                builder_.CreateStore(fieldVal, slot);
                locals_[vbp] = slot;
            } else if (sp.pattern) {
                // Non-binding sub-pattern: check it against the field value.
                // §19.3 / fix #615: resolve the field's declared type so that
                // enum-constant NamedPatterns and string ConstantPatterns get
                // the same enumTy / isStr arguments that a top-level pattern
                // match would receive.  Without these, enum sub-patterns always
                // returned nullptr (enumTy==nullptr → NamedPattern fell through
                // → AND skipped → always matched) and string sub-patterns used
                // pointer equality instead of content equality.
                const sema::TypeRef* fieldTy = recordMemberType(fields[i]);
                const sema::TypeSymbol* subEnumTy = nullptr;
                bool subIsStr = false;
                if (fieldTy && fieldTy->kind == sema::TypeRef::Kind::Named &&
                    fieldTy->declaration &&
                    fieldTy->declaration->kind == ast::TypeDeclKind::Enum) {
                    subEnumTy = fieldTy->declaration;
                } else if (fieldTy &&
                           fieldTy->kind == sema::TypeRef::Kind::Primitive &&
                           fieldTy->primitive == sema::PrimitiveType::String) {
                    subIsStr = true;
                }
                llvm::Value* subMatch =
                    patternMatches(sp.pattern.get(), fieldVal,
                                   subEnumTy, subIsStr, fieldTy);
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
    const sema::TypeRef* resultSemaType = ctx_.exprType(c);
    if (resultSemaType) resultTy = lowerTypeRef(resultSemaType);

    auto* thenBB = llvm::BasicBlock::Create(llvm_, "cond.then", currentFn_);
    auto* elseBB = llvm::BasicBlock::Create(llvm_, "cond.else", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "cond.end", currentFn_);
    builder_.CreateCondBr(cond, thenBB, elseBB);

    // Lower the then branch. Do NOT emit the branch-to-endBB terminator yet —
    // nullable widening (below) may need to emit instructions in this block
    // after we've seen both branches' LLVM types.
    builder_.SetInsertPoint(thenBB);
    llvm::Value* tv = lowerExpr(*c.thenValue);
    bool thenDiv = diverged_;  // CG40: e.g. `cond ? throw E : b`
    if (tv && resultTy && !thenDiv)
        tv = coerceNumericToType(tv, resultTy,
                                 isUnsignedTargetExpr(*c.thenValue));
    llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
    // (terminator CreateBr(endBB) deferred — emitted after widening below)

    builder_.SetInsertPoint(elseBB);
    llvm::Value* ev = lowerExpr(*c.elseValue);
    bool elseDiv = diverged_;
    if (ev && resultTy && !elseDiv)
        ev = coerceNumericToType(ev, resultTy,
                                 isUnsignedTargetExpr(*c.elseValue));
    llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();
    // (terminator CreateBr(endBB) deferred — emitted after widening below)

    // §17 / §7: nullable value-type widening.
    //
    // A conditional `(V if C else null)` whose result is a nullable value
    // type (int?, bool?, double?, enum?, nullable record) produces a PHI
    // with a type mismatch: the non-null branch emits a bare T (e.g. i32)
    // while the null-literal branch emits a ConstantPointerNull (because
    // sema types the `null` literal as nullable Object — a reference type —
    // rather than the specific nullable value type). Both PHI operands must
    // share the tagged {i8 hasValue; T value} struct type.
    //
    // Two complementary paths reconcile this:
    //
    // Path A — sema result type IS a nullable value type (e.g. the sema
    // conditional type is already int?). Widen each branch to the sema-
    // provided nullable struct, reusing the existing helpers.
    //   • bare T → wrapNullableValue (hasValue=1, T)
    //   • ConstantPointerNull → nullableValueNull (hasValue=0, T=0)
    //   • already the nullable struct → pass through.
    //
    // Path B — sema result type is a REFERENCE type (e.g. Object? from the
    // null-literal's default type) but the branch values are mismatched:
    // one is a ConstantPointerNull and the other is a non-ptr value (bare T
    // or an existing nullable struct). Derive the nullable struct from the
    // non-null branch's LLVM type and widen both.
    //   • ConstantPointerNull → ConstantAggregateZero({i8, T}) — no instrs.
    //   • bare T → InsertValue({i8 1, T}) — emitted in that branch's block.
    //   • already a {i8, T} struct → pass through unchanged.
    //
    // In both paths, widening instructions for the non-null branch are
    // emitted INSIDE THAT BRANCH'S BLOCK (before its terminator), so the
    // result dominates the PHI edge. The branch terminators (CreateBr) are
    // therefore deferred until after widening.
    //
    // Nullable REFERENCE type branches (non-null ptr + null ptr) already
    // share the pointer type — neither path fires for them.
    //
    // per §17 (conditional expression) / §7.1 (nullable types) / §7.2 (null).
    if (tv && ev && !thenDiv && !elseDiv) {
        bool needPathA = resultSemaType && isNullableValueTy(resultSemaType);
        if (needPathA) {
            // Path A: sema-guided widening to the nullable struct.
            llvm::StructType* nst = nullableValueStructTy(resultSemaType);

            // Widen a single branch value to nst. Instructions (if any) are
            // emitted into the block that is current when this is called, so
            // call inside the branch's own block.
            auto widenOne = [&](llvm::Value* v) -> llvm::Value* {
                if (!v) return v;
                if (v->getType() == nst) return v;  // already the struct
                if (v->getType()->isPointerTy() &&
                    llvm::isa<llvm::ConstantPointerNull>(v))
                    return nullableValueNull(resultSemaType);  // constant
                if (v->getType() == nst->getElementType(1))
                    return wrapNullableValue(resultSemaType, v);  // inserts
                return v;  // unrecognised — leave, mismatch caught elsewhere
            };

            // Widen thenEnd (emit in thenEnd's block).
            builder_.SetInsertPoint(thenEnd);
            tv = widenOne(tv);
            // Widen elseEnd (emit in elseEnd's block).
            builder_.SetInsertPoint(elseEnd);
            ev = widenOne(ev);

        } else if (tv->getType() != ev->getType()) {
            // Path B: no sema-supplied nullable struct; detect ptr-null vs
            // non-ptr mismatch and reconcile using the non-null branch's type.
            auto isNullPtr = [](llvm::Value* v) {
                return v->getType()->isPointerTy() &&
                       llvm::isa<llvm::ConstantPointerNull>(v);
            };

            // widenPair: widen nullBranch (ptr null) and valBranch (the
            // non-null side) to a common {i8, T} struct. Wrapping instrs
            // are emitted into valBlock (the branch block for valBranch).
            auto widenPair = [&](llvm::Value*& nullBranch,
                                 llvm::Value*& valBranch,
                                 llvm::BasicBlock* valBlock) {
                llvm::Type* innerTy = valBranch->getType();
                llvm::StructType* nst;
                if (auto* st = llvm::dyn_cast<llvm::StructType>(innerTy);
                    st && st->getNumElements() == 2 &&
                    st->getElementType(0) == nullablePrimHasValTy()) {
                    // valBranch is already a nullable struct — use it directly.
                    nst = st;
                } else {
                    // valBranch is a bare T — wrap into {i8 1, T}.
                    nst = llvm::StructType::get(
                        llvm_, {nullablePrimHasValTy(), innerTy});
                    builder_.SetInsertPoint(valBlock);
                    llvm::Value* res = builder_.CreateInsertValue(
                        llvm::UndefValue::get(nst),
                        llvm::ConstantInt::get(nullablePrimHasValTy(), 1),
                        {0u});
                    valBranch = builder_.CreateInsertValue(res, valBranch, {1u});
                }
                // Null branch: all-zero constant — no instruction needed.
                nullBranch = llvm::ConstantAggregateZero::get(nst);
            };

            if (isNullPtr(ev) && !isNullPtr(tv)) {
                widenPair(ev, tv, thenEnd);   // then=value, else=null
            } else if (isNullPtr(tv) && !isNullPtr(ev)) {
                widenPair(tv, ev, elseEnd);   // then=null, else=value
            }
            // Both ptr nulls or neither: leave them — the PHI mismatch
            // reflects a pre-existing sema error and is not our concern.
        }
    }

    // Emit branch terminators now that widening (if any) is complete.
    // SetInsertPoint restores the correct block before each CreateBr.
    if (!thenDiv) {
        builder_.SetInsertPoint(thenEnd);
        builder_.CreateBr(endBB);
    }
    if (!elseDiv) {
        builder_.SetInsertPoint(elseEnd);
        builder_.CreateBr(endBB);
    }

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
