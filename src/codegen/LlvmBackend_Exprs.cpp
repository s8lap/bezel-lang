#include "codegen/LlvmBackend_impl.hpp"

// §4.4 fix #521: decimal literal compile-time parse.
// bz_decimal_parse_cstr is compiled into the same process (the runtime is linked
// into bezel_tests and also available in the driver binary).  Calling it from the
// compiler during code generation is safe for both AOT and JIT modes.
// per fix #521: ove_vtable/fe_vtable are nullptr here because literals are
// already validated by the lexer; the cstr call cannot fail at compile time.
extern "C" void bz_decimal_parse_cstr(const char* s, void* out,
                                       void* ove_vtable, void* fe_vtable);

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
            bz_decimal_parse_cstr(digits.c_str(), dec, nullptr, nullptr);
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
                // R36/#733: for a VERBATIM interpolated string (`$@"..."`, §18.7)
                // the literal parts must NOT have escapes decoded — the splitter
                // already kept the raw backslash. Thread isVerbatim through AND
                // wrap with the matching `@"` delimiter so processStringCodePoints'
                // verbatim strip (start = verbatim ? 2 : 1) lines up exactly as the
                // plain @"..." path does; otherwise `$@"x\ny"` decoded `\n` → LF.
                std::string wrapped =
                    (is->isVerbatim ? std::string("@\"") : std::string("\"")) +
                    part.literal + "\"";
                std::string content =
                    processStringText(wrapped, is->isVerbatim);
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
                    auto* frt = ctx_.exprType(*part.expr);
                    // §18.5 / §7: a nullable VALUE part WITH a format spec —
                    // branch on hasValue (absent → "null"; present → apply the
                    // spec to the UNWRAPPED payload). emitWithFormatSpec expects
                    // the bare primitive, not the {i8, T} tagged struct: feeding
                    // it the struct sign-extended a struct (`{i8,i32}` → i64) or
                    // dropped the spec (#849).
                    if (frt && isNullableValueTy(frt) &&
                        ev->getType()->isStructTy()) {
                        llvm::Value* hasVal = nullablePrimHasValue(ev);
                        llvm::Value* primVal = nullablePrimValue(ev);
                        auto* valBB = llvm::BasicBlock::Create(
                            llvm_, "fs.val", currentFn_);
                        auto* nullBB = llvm::BasicBlock::Create(
                            llvm_, "fs.null", currentFn_);
                        auto* endBB = llvm::BasicBlock::Create(
                            llvm_, "fs.end", currentFn_);
                        builder_.CreateCondBr(hasVal, valBB, nullBB);
                        builder_.SetInsertPoint(valBB);
                        sema::TypeRef innerTy = *frt;
                        innerTy.nullable = false;
                        llvm::Value* vStr = emitWithFormatSpec(
                            primVal, part.formatSpec, &innerTy);
                        if (!vStr) vStr = emitValueToString(primVal, &innerTy);
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
                        appendPart(phi);
                        continue;
                    }
                    pv = emitWithFormatSpec(ev, part.formatSpec, frt);
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
                } else if (rt && isNullableRefTy(rt) &&
                           ev->getType()->isPointerTy()) {
                    // R35: a nullable REFERENCE part (`string?`, `Object?`,
                    // nullable class/interface) is a plain ptr with a real null.
                    // Interpolating null must render the literal "null", not
                    // dereference it (the `string?` path fell into the Primitive
                    // branch below and emitPrimitiveToString read a null string's
                    // header → SIGSEGV; a `class?` reached the ToString path and
                    // called a method on null). Mirror emitValueToString's
                    // null-ref handling: null → "null"; non-null → format by the
                    // non-nullable rule.
                    llvm::Value* isNull = builder_.CreateICmpEQ(
                        ev, llvm::ConstantPointerNull::get(ptrTy()), "ip.null");
                    auto* liveBB =
                        llvm::BasicBlock::Create(llvm_, "ip.live", currentFn_);
                    auto* nullBB =
                        llvm::BasicBlock::Create(llvm_, "ip.nullstr", currentFn_);
                    auto* endBB =
                        llvm::BasicBlock::Create(llvm_, "ip.end", currentFn_);
                    builder_.CreateCondBr(isNull, nullBB, liveBB);
                    builder_.SetInsertPoint(liveBB);
                    sema::TypeRef liveTy = *rt;
                    liveTy.nullable = false;
                    llvm::Value* liveStr = emitValueToString(ev, &liveTy);
                    auto* liveEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(nullBB);
                    llvm::Value* nullStr = lowerStringLiteral("\"null\"", false);
                    auto* nullEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(endBB);
                    auto* phi = builder_.CreatePHI(ptrTy(), 2);
                    phi->addIncoming(liveStr, liveEnd);
                    phi->addIncoming(nullStr, nullEnd);
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
        // R33: the operand may already be the nullable-value struct
        // {i8 hasValue; <T> value} even when both isNullableValueTy and the
        // #604 type-param resolution above miss it — e.g. a nullable
        // type-parameter FIELD `_v: T?` (the field's TypeRef carries a
        // declaration, so the #604 guard `!rt->declaration` does not fire).
        // Without this, `_v!` returns the whole struct and a `return _v!`
        // emits `ret { i8, i32 }` against an `i32` function → IR-verify
        // failure.  Detect by LLVM shape (element 0 is the hasValue tag) and
        // extract the value slot, exactly as the null-comparison path does.
        if (v->getType()->isStructTy()) {
            auto* st = llvm::cast<llvm::StructType>(v->getType());
            if (st->getNumElements() >= 1 &&
                st->getElementType(0) == nullablePrimHasValTy()) {
                emitGuardThrow(builder_.CreateNot(nullablePrimHasValue(v)),
                               "NullReferenceException");
                return nullablePrimValue(v);
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
                    // #730: a receiver of bare type-parameter type (`a: T` in a
                    // monomorphized generic body, e.g. `where T: IComparable<T>`
                    // with T=int calling `a.CompareTo(b)`) resolves to its
                    // concrete type via the active substitution. When the param
                    // binds to a PRIMITIVE, substitute it so the primitive-method
                    // paths below (CompareTo, etc.) fire — instead of falling
                    // through to an interface dispatch that returned 0.
                    if (tt && tt->kind == sema::TypeRef::Kind::Named &&
                        !tt->declaration && !genericSubstSemaTy_.empty() &&
                        tt->astNode && tt->astNode->name &&
                        tt->astNode->name->parts.size() == 1) {
                        auto it = genericSubstSemaTy_.find(
                            tt->astNode->name->parts[0]);
                        if (it != genericSubstSemaTy_.end() && it->second &&
                            it->second->kind == sema::TypeRef::Kind::Primitive)
                            tt = const_cast<sema::TypeRef*>(it->second);
                    }
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
                            // #429 (OPERATOR-OPERAND category) / §4.6 / §28.9:
                            // pre-lower the receiver so we can release it after
                            // the method call when it is a +1 string producer
                            // (concat / interpolation / primitive-ToString /
                            // user-method-return / range-slice). The receiver is
                            // consumed by the method (only read; it is a borrow
                            // inside tryLowerStringMethod), and has no named-local
                            // owner — nothing else releases it.  Only pre-lower when
                            // the target IS a +1 producer, so non-producer receivers
                            // still flow through the existing path inside
                            // tryLowerStringMethod without any change.
                            bool selfIsProducer =
                                ma->target &&
                                isPlusOneStringProducer(ma->target.get());
                            llvm::Value* selfV =
                                selfIsProducer ? lowerExpr(*ma->target) : nullptr;
                            if (auto* v = tryLowerStringMethod(*call, *ma, selfV)) {
                                // Release the +1 producer receiver AFTER the
                                // method has been lowered (result is in hand;
                                // string still live for the method's reads).
                                if (selfIsProducer && selfV)
                                    emitStringRelease(selfV);
                                return v;
                            }
                            // tryLowerStringMethod returned nullptr (method not
                            // recognised by the structurally-matched §9.6 paths).
                            // If we pre-lowered the receiver, it is harmless to
                            // proceed: tryLowerStringMethod re-lowers when selfV
                            // is null, but since we passed selfV it skipped the
                            // lowerExpr call — so it just returned nullptr without
                            // side-effects. The fall-through paths below do NOT
                            // lower ma->target again for a string receiver type
                            // (they only apply to non-string primitives).
                            // Releasing selfV on the nullptr path is SAFE: nothing
                            // else will use the lowered value since we are about
                            // to return nullptr anyway. This avoids a leak if the
                            // receiver was a producer and tryLowerStringMethod
                            // happened to not recognise the method.
                            if (selfIsProducer && selfV)
                                emitStringRelease(selfV);
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
                // R33: the index may be a Range *value* (a variable / call
                // result of type Range) rather than a RangeExpr literal — e.g.
                // `var r := 0..2; s[r]`. It lowers to the %bz.range struct, so
                // the single-char path below would sext the whole struct → an
                // IR-verify failure. Detect the range-typed operand and slice
                // from the struct fields.
                if (auto* idxTy = ctx_.exprType(*ix->indices[0].value)) {
                    if (isRangeType(idxTy)) {
                        llvm::Value* strPtr = lowerExpr(*ix->target);
                        llvm::Value* rv = lowerExpr(*ix->indices[0].value);
                        if (!strPtr || !rv) return nullptr;
                        return emitStringRangeSliceValue(strPtr, rv);
                    }
                }
                // §4.6: a single int index `s[i]` returns the i-th char.
                llvm::Value* strPtr = lowerExpr(*ix->target);
                llvm::Value* idx = lowerExpr(*ix->indices[0].value);
                if (!strPtr || !idx) return nullptr;
                llvm::Value* idx64 = idx->getType() == i64() ? idx
                    : builder_.CreateSExt(idx, i64(), "str.idx64");
                // §4.1 / §4.6 / §31.2 (R33): bounds-check the index. Without
                // this `s[1000000]` (or a negative index) read out of bounds and
                // SEGFAULTed; the spec requires IndexOutOfRangeException. Use an
                // unsigned compare so a sign-extended negative index (a huge
                // unsigned value) is rejected too — mirrors the inline-array and
                // slice paths.
                {
                    llvm::Value* slen = loadStringLen(strPtr);
                    llvm::Value* oor =
                        builder_.CreateICmpUGE(idx64, slen, "str.oor");
                    emitGuardThrow(oor, "IndexOutOfRangeException");
                }
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
    // §27.7 (#738): null-conditional invocation `f?(args)`. Invoke the nullable
    // function value when non-null, else skip; the result is the function's
    // return type made NULLABLE (null when f was null). Was unimplemented
    // (NullCondCallExpr produced a wrong value).
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        // §5.3 (#839): T?(x) — nullable-target primitive cast. The lexer fuses
        // `?` and `(` into the two-char `?(` punctuator, so `long?(x)` is
        // parsed as NullCondCallExpr{callee=IdentifierExpr{"long"}, args=[x]}
        // rather than a CallExpr.  When the callee is a bare primitive-type
        // keyword with no symbol this is NOT a null-conditional function call
        // but a §5.3 LIFTED CAST: null stays null, a value is cast to the
        // target primitive and re-wrapped in the nullable struct.
        if (nc->callee && nc->args.size() == 1 && nc->args[0].value) {
            if (auto* cid =
                    dynamic_cast<const ast::IdentifierExpr*>(nc->callee.get())) {
                if (!ctx_.exprSymbol(*nc->callee)) {
                    using P = sema::PrimitiveType;
                    P targetPt = sema::primitiveFromName(cid->name);
                    if (targetPt != P::None && targetPt != P::String &&
                        targetPt != P::Decimal) {
                        const ast::Expr& argE = *nc->args[0].value;
                        llvm::Value* srcV = lowerExpr(argE);
                        if (!srcV) return nullptr;
                        // Determine source primitive and whether it is nullable.
                        P srcPt = P::None;
                        bool srcNullable = false;
                        if (auto* rt = ctx_.exprType(argE)) {
                            if (rt->kind == sema::TypeRef::Kind::Primitive)
                                srcPt = rt->primitive;
                            srcNullable = rt->nullable;
                        }
                        // Extract hasValue + payload from a nullable source, or
                        // treat a non-nullable source as hasValue=true, payload=src.
                        llvm::Value* hasV;
                        llvm::Value* payload;
                        if (srcNullable && srcV->getType()->isStructTy() &&
                            isNullableValueTy(ctx_.exprType(argE))) {
                            hasV    = nullablePrimHasValue(srcV);  // i1
                            payload = nullablePrimValue(srcV);     // inner scalar
                        } else {
                            hasV    = builder_.getTrue();
                            payload = srcV;
                        }
                        // Convert the payload from source primitive to target.
                        // For an identity cast (same primitive) lowerPrimCastScalar
                        // returns the payload unchanged.
                        bool srcUns = (srcPt != P::None) ? isUnsignedPrim(srcPt)
                                                         : false;
                        llvm::Value* converted =
                            lowerPrimCastScalar(targetPt, srcPt, payload, srcUns);
                        if (!converted) return nullptr;
                        // Re-wrap as {i8 hasValue, targetScalar} nullable struct.
                        // Per §5.3: null stays null (hasV=0); value is re-wrapped
                        // with the same hasV (1) from the source.
                        llvm::StructType* st = nullablePrimStructTy(targetPt);
                        llvm::Value* hv8 =
                            builder_.CreateZExt(hasV, nullablePrimHasValTy(),
                                                "nc.hv8");
                        llvm::Value* res = builder_.CreateInsertValue(
                            llvm::UndefValue::get(st), hv8, {0u});
                        return builder_.CreateInsertValue(res, converted, {1u},
                                                          "nc.cast");
                    }
                }
            }
        }
        const sema::TypeRef* fnRt =
            nc->callee ? ctx_.exprType(*nc->callee) : nullptr;
        if (!fnRt || fnRt->kind != sema::TypeRef::Kind::Function ||
            !fnRt->funcReturn)
            return nullptr;
        llvm::Value* closurePtr = lowerExpr(*nc->callee);
        if (!closurePtr || !closurePtr->getType()->isPointerTy()) return nullptr;
        llvm::Type* rTy = lowerTypeRef(fnRt->funcReturn);  // R
        bool refResult = rTy->isPointerTy();
        // fix #747 R43 / §27.7: a null-conditional call on a VOID-returning
        // function (`((int) -> ())?`) is a statement-position no-op when null
        // and discards the result otherwise — there is no "nullable void" value
        // to materialize. The general path below built a `{i8, void}` nullable
        // struct and a PHI over it, which is malformed and aborted the compiler
        // (PHINode type-mismatch assertion). Emit the guarded call with no
        // result PHI and return no value (Unit), like an ordinary void call.
        bool voidResult = rTy->isVoidTy();
        // Result type R?: ptr for a reference R, tagged {i8 hasValue; R} struct
        // for a value R (matches lowerTypeRef of the sema-set R? result).
        llvm::Type* resTy = refResult
            ? static_cast<llvm::Type*>(ptrTy())
            : static_cast<llvm::Type*>(
                  llvm::StructType::get(llvm_, {nullablePrimHasValTy(), rTy}));
        auto* entryBB = builder_.GetInsertBlock();
        auto* callBB = llvm::BasicBlock::Create(llvm_, "nccall.do", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "nccall.end", currentFn_);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            closurePtr, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(isNull, endBB, callBB);

        builder_.SetInsertPoint(callBB);
        // Closure ABI { ptr fn, ptr env }: call fn(env, user args...).
        llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closurePtr);
        llvm::Value* envSlot =
            builder_.CreateGEP(ptrTy(), closurePtr, {builder_.getInt32(1)});
        llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot);
        std::vector<llvm::Value*> args;
        args.push_back(envPtr);
        for (const auto& a : nc->args) {
            if (!a.value) return nullptr;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return nullptr;
            args.push_back(v);
        }
        std::vector<llvm::Type*> argTys;
        for (auto* v : args) argTys.push_back(v->getType());
        auto* fnTy = llvm::FunctionType::get(rTy, argTys, false);
        // A void call must not carry a value name (LLVM forbids naming a void
        // instruction); only name the result when it is a real value.
        llvm::Value* callRes = builder_.CreateCall(
            fnTy, fnPtr, args, voidResult ? "" : "nccall.ret");
        // Wrap the non-void result into R? in THIS (call) block so it dominates
        // its PHI edge; the void case has no result to wrap.
        llvm::Value* wrapped =
            voidResult ? nullptr
            : (refResult ? callRes
                         : wrapNullableValue(fnRt->funcReturn, callRes));
        auto* callEndBB = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        if (voidResult) return nullptr;  // Unit result — no PHI.
        auto* phi = builder_.CreatePHI(resTy, 2);
        llvm::Value* nullVal = refResult
            ? static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(ptrTy()))
            : nullableValueNull(fnRt->funcReturn);
        phi->addIncoming(nullVal, entryBB);
        phi->addIncoming(wrapped, callEndBB);
        return phi;
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
                            // stdlib §char: MaxValue = U+10FFFF (char is a 32-bit
                            // code point, lowered to i32). #747 R42: was missing,
                            // so the read hit an uninitialized alloca.
                            case P::Char: return llvm::ConstantInt::get(i32(), 1114111, false);
                            default: break;
                        }
                    }
                    if (mbr == "MinValue") {
                        switch (pt) {
                            case P::Int:  return llvm::ConstantInt::get(i32(), int32_t(-2147483648ll), true);
                            case P::Long: return llvm::ConstantInt::get(i64(), int64_t(-9223372036854775808ll), true);
                            case P::Short:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), int16_t(-32768), true);
                            case P::SByte:return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), int8_t(-128), true);
                            // stdlib §char: MinValue = U+0000 (0). #747 R42.
                            case P::UInt: case P::ULong: case P::UShort: case P::Byte: case P::Char:
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
                    // #429 (OPERATOR-OPERAND category) / §4.6 / §28.9: if the
                    // receiver is a +1 string producer (concat, interpolation,
                    // primitive-ToString, user-method-return, range-slice), it has
                    // no named-local owner and no scope-end release — release it
                    // here, AFTER loading the length, so the string lives long
                    // enough for the length-field read.  bz_string_release on a
                    // sentinel is a no-op (safe for literal receivers).
                    if (isPlusOneStringProducer(ma->target.get()))
                        emitStringRelease(strPtr);
                    return builder_.CreateTrunc(len64, i32(), "str.len.i32");
                }
                // stdlib §9.6 (#741): IsAscii — true iff every code point < 128.
                // No codegen case existed, so the read returned an uninitialized
                // i1 slot (always false). Scan via bz_string_is_ascii.
                if (ma->member == "IsAscii") {
                    auto* fn = declareC("bz_string_is_ascii", i32(), {ptrTy()});
                    llvm::Value* r = builder_.CreateCall(fn, {strPtr}, "str.ascii");
                    if (isPlusOneStringProducer(ma->target.get()))
                        emitStringRelease(strPtr);
                    return builder_.CreateICmpNE(r, builder_.getInt32(0),
                                                 "str.isascii");
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
            // Exclude StringBuilder (has its own handler that sums part lengths)
            // and Buffer<T> (has its own header-load handler below — the string
            // fallback would call loadStringLen which loads field[1]=elem_size
            // instead of field[0]=len, producing a wrong value per issue #545).
            auto isStringFallbackTarget = [&]() -> bool {
                if (!tt) return true;  // unknown type → fallback
                if (tt->kind != sema::TypeRef::Kind::Named || tt->declaration)
                    return false;
                if (!tt->astNode || !tt->astNode->name ||
                    tt->astNode->name->parts.empty())
                    return true;
                std::string_view n = tt->astNode->name->parts[0];
                if (n == "StringBuilder") return false;  // has own handler
                if (n == "Buffer") return false;          // issue #545: own handler
                return true;
            };
            if (ma->member == "Length" && isStringFallbackTarget() &&
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
            // §8.1 / issue #646: Stopwatch instance property reads.
            // Receiver is a bz_stopwatch_t* (heap pointer).
            if (cn == "Stopwatch") {
                llvm::Value* sw = lowerExpr(*ma->target);
                if (!sw) return nullptr;
                if (ma->member == "IsRunning") {
                    auto* fn = declareC("bz_stopwatch_is_running", i32(), {ptrTy()});
                    llvm::Value* flag = builder_.CreateCall(fn, {sw}, "sw.running");
                    return builder_.CreateICmpNE(flag, builder_.getInt32(0), "sw.running.b");
                }
                if (ma->member == "ElapsedMilliseconds") {
                    auto* fn = declareC("bz_stopwatch_elapsed_ms", i64(), {ptrTy()});
                    return builder_.CreateCall(fn, {sw}, "sw.elms");
                }
                if (ma->member == "Elapsed") {
                    // Returns TimeSpan (i64, 100-ns ticks).
                    auto* fn = declareC("bz_stopwatch_elapsed_ticks", i64(), {ptrTy()});
                    return builder_.CreateCall(fn, {sw}, "sw.el");
                }
            }
            // §6.4 / issue #647: Atomic<T> and AtomicInteger<T> — Value getter.
            // `atom.Value` → bz_atomic_load(atom, cell_size).
            // T is in genericArgs[0] of the receiver's TypeRef.
            if ((cn == "Atomic" || cn == "AtomicInteger") &&
                ma->member == "Value") {
                llvm::Value* atom = lowerExpr(*ma->target);
                if (!atom) return nullptr;
                // Determine cell_size from T.
                int cellSize = 4;
                if (tt && !tt->genericArgs.empty() && tt->genericArgs[0]) {
                    const sema::TypeRef* tArg = tt->genericArgs[0];
                    if (tArg->kind == sema::TypeRef::Kind::Primitive) {
                        using P = sema::PrimitiveType;
                        switch (tArg->primitive) {
                            case P::Bool: case P::Byte: case P::SByte:
                                cellSize = 1; break;
                            case P::Short: case P::UShort: cellSize = 2; break;
                            case P::Long: case P::ULong:   cellSize = 8; break;
                            default: cellSize = 4; break;
                        }
                    } else {
                        cellSize = 8;  // IntPtr/UIntPtr
                    }
                }
                auto* loadFn = declareC("bz_atomic_load", i64(), {ptrTy(), i32()});
                llvm::Value* raw = builder_.CreateCall(
                    loadFn, {atom, builder_.getInt32(cellSize)}, "atom.load");
                // Truncate to the declared T width.
                llvm::Type* tTy = i32();
                if (tt && !tt->genericArgs.empty() && tt->genericArgs[0])
                    tTy = lowerTypeRef(tt->genericArgs[0]);
                if (!tTy) tTy = i32();
                if (tTy->isIntegerTy() && !tTy->isIntegerTy(64))
                    return builder_.CreateTrunc(raw, tTy, "atom.val");
                return raw;  // i64 or bool (i1 from trunc handled above)
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
            // stdlib §3.2 v0.136: Encoding.Utf8.Name / Utf16.Name / Utf32.Name /
            // Ascii.Name — read-only property returning the IANA encoding label.
            // The shape is: ma->member == "Name", ma->target is a
            // MemberAccessExpr(target=Id("Encoding"), member=<singleton>).
            if (ma->member == "Name") {
                if (auto* inner2 =
                        dynamic_cast<const ast::MemberAccessExpr*>(
                            ma->target.get())) {
                    if (auto* encId =
                            dynamic_cast<const ast::IdentifierExpr*>(
                                inner2->target.get())) {
                        if (encId->name == "Encoding") {
                            const char* nameStr = nullptr;
                            if (inner2->member == "Utf8")  nameStr = "utf-8";
                            if (inner2->member == "Utf16") nameStr = "utf-16le";
                            if (inner2->member == "Utf32") nameStr = "utf-32le";
                            if (inner2->member == "Ascii") nameStr = "us-ascii";
                            if (nameStr) {
                                // Emit as a string literal (constant global).
                                // Wrap in quotes — lowerStringLiteral strips them.
                                return lowerStringLiteral(
                                    std::string("\"") + nameStr + "\"", false);
                            }
                        }
                    }
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
                // stdlib §9.3 / fix #521: Decimal static constants.
                if (id2->name == "Decimal" || id2->name == "decimal") {  // §5: both spellings (spec L243)
                    llvm::StructType* dst = decimalStructType();
                    const auto& mbr = ma->member;
                    if (mbr == "Zero")
                        return llvm::ConstantAggregateZero::get(dst);
                    if (mbr == "One") {
                        uint32_t dec[4] = {0, 0, 0, 0};
                        bz_decimal_parse_cstr("1", dec, nullptr, nullptr);
                        return llvm::ConstantStruct::get(dst, {
                            llvm::ConstantInt::get(i32(), dec[0]),
                            llvm::ConstantInt::get(i32(), dec[1]),
                            llvm::ConstantInt::get(i32(), dec[2]),
                            llvm::ConstantInt::get(i32(), dec[3])
                        });
                    }
                    if (mbr == "MinValue") {
                        uint32_t dec[4] = {0, 0, 0, 0};
                        bz_decimal_parse_cstr("-79228162514264337593543950335", dec, nullptr, nullptr);
                        return llvm::ConstantStruct::get(dst, {
                            llvm::ConstantInt::get(i32(), dec[0]),
                            llvm::ConstantInt::get(i32(), dec[1]),
                            llvm::ConstantInt::get(i32(), dec[2]),
                            llvm::ConstantInt::get(i32(), dec[3])
                        });
                    }
                    if (mbr == "MaxValue") {
                        uint32_t dec[4] = {0, 0, 0, 0};
                        bz_decimal_parse_cstr("79228162514264337593543950335", dec, nullptr, nullptr);
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
                if (tt->arraySizeValue >= 0)  // #734: folded member-const size
                    return llvm::ConstantInt::get(i32(), tt->arraySizeValue,
                                                  true);
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
    // fix #667 / §30.15 / stdlib §6.9: Channel<T>.IsClosed and Channel<T>.Count
    // as property reads (MemberAccessExpr without parens). These are "readonly
    // property" in the stdlib, so the parser generates a MemberAccessExpr (not
    // a CallExpr), bypassing tryLowerChannelAndDomainCall.  Handle them here so
    // the MemberAccess path correctly calls bz_channel_is_closed /
    // bz_channel_count.  The null-guards in those runtime functions (fix #667)
    // ensure that null (post-Dispose slot) returns the expected sentinel values.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target &&
            (ma->member == "IsClosed" || ma->member == "Count")) {
            auto* tt = ctx_.exprType(*ma->target);
            bool isChProp = (tt && tt->kind == sema::TypeRef::Kind::Named &&
                             !tt->declaration && tt->astNode &&
                             tt->astNode->name &&
                             !tt->astNode->name->parts.empty() &&
                             tt->astNode->name->parts[0] == "Channel");
            if (isChProp) {
                llvm::Value* ch = lowerExpr(*ma->target);
                if (ch) {
                    if (ma->member == "IsClosed") {
                        auto* fn = declareC("bz_channel_is_closed",
                                           i32(), {ptrTy()});
                        llvm::Value* r = builder_.CreateCall(
                            fn, {ch}, "ch.isclosed");
                        return builder_.CreateICmpNE(r, builder_.getInt32(0));
                    } else {
                        auto* fn = declareC("bz_channel_count",
                                           i64(), {ptrTy()});
                        llvm::Value* c = builder_.CreateCall(fn, {ch});
                        return builder_.CreateTrunc(c, i32(), "ch.count.i");
                    }
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
            // issue #545: Buffer<T>.Length → i64 header field load.
            // The header layout is { i64 len, i64 elem_size } (bz_buffer_hdr_t).
            // GEP field[0] from the buffer pointer and load as i64, then trunc to
            // i32 (Bezel `int`) for consistency with other collection Length values.
            {
                bool isBuf = tt && tt->kind == sema::TypeRef::Kind::Named &&
                             !tt->declaration && tt->astNode && tt->astNode->name &&
                             !tt->astNode->name->parts.empty() &&
                             tt->astNode->name->parts[0] == "Buffer";
                if (isBuf && ma->member == "Length") {
                    llvm::Value* obj = lowerExpr(*ma->target);
                    if (obj) {
                        // bz_buffer_hdr_t: { int64 len, int64 elem_size }.
                        // The `len` field is at byte offset 0.
                        // Use a GEP into the raw bytes (i8*) at offset 0 and
                        // load as i64 to avoid struct-type aliasing surprises.
                        llvm::Value* len64 =
                            builder_.CreateLoad(i64(), obj, "buf.len");
                        return builder_.CreateTrunc(len64, i32(), "buf.len.i32");
                    }
                }
            }
            // §4.14 / §9.8 / issue #550: OwnedBuffer<T>.Count → bz_array_len.
            // OwnedBuffer is array-backed (bz_array_t*) but is not in
            // isCollectionTypeName (which lists element-owning collections).
            {
                bool isOB = tt && tt->kind == sema::TypeRef::Kind::Named &&
                            !tt->declaration && tt->astNode && tt->astNode->name &&
                            !tt->astNode->name->parts.empty() &&
                            tt->astNode->name->parts[0] == "OwnedBuffer";
                if (isOB) {
                    llvm::Value* obj = lowerExpr(*ma->target);
                    if (obj) {
                        auto* fn = declareC("bz_array_len", i64(), {ptrTy()});
                        llvm::Value* cnt64 = builder_.CreateCall(fn, {obj});
                        return builder_.CreateTrunc(cnt64, i32(), "ob.cnt");
                    }
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
    // stdlib §4.4/§4.5/§4.14 (#741): `.Capacity` on a `bz_array_t`-backed type
    // → bz_array_capacity. The spec declares `Capacity` only on Array/OwnedArray
    // (and OwnedBuffer exposes it as `Length`), but every bz_array_t-backed
    // collection has a valid backing capacity; the allowlist mirrors the
    // construction branch (LlvmBackend_Exprs.cpp ~L6004). LinkedList (bz_llist)
    // and Dictionary (bz_dict) use different backings and are NOT included. No
    // codegen case existed, so the read previously returned an uninitialized slot.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Capacity") {
            auto* tt = ctx_.exprType(*ma->target);
            auto headName = [&](const sema::TypeRef* t) -> std::string_view {
                return (t && t->astNode && t->astNode->name &&
                        !t->astNode->name->parts.empty())
                           ? t->astNode->name->parts[0] : std::string_view{};
            };
            std::string_view cn = headName(tt);
            bool isArrayBacked =
                cn == "Array" || cn == "OwnedArray" || cn == "OwnedBuffer" ||
                cn == "Stack" || cn == "OwnedStack" ||
                cn == "Queue" || cn == "OwnedQueue" ||
                cn == "Set"   || cn == "OwnedSet";
            if (isArrayBacked) {
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj) {
                    auto* fn = declareC("bz_array_capacity", i64(), {ptrTy()});
                    llvm::Value* cap64 = builder_.CreateCall(fn, {obj});
                    return builder_.CreateTrunc(cap64, i32(), "col.cap");
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
                // issue #759: async enumerators share the slot-3 Current layout.
                (recvTy->astNode->name->parts[0] == "IEnumerator" ||
                 recvTy->astNode->name->parts[0] == "OwnedEnumerator" ||
                 recvTy->astNode->name->parts[0] == "IAsyncEnumerator" ||
                 recvTy->astNode->name->parts[0] == "OwnedAsyncEnumerator");
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
    // §27.6 (#756): a property reference in FUNCTION-VALUE position. sema's
    // target-typed promotion (checkAssignable) typed `b.Count` as `() -> T` when
    // it is bound to a function-typed slot; produce the getter as a closure
    // (≡ `() => b.Count`) instead of evaluating the getter here (which would
    // store the int into a function-pointer slot and SIGSEGV at the call). Gate
    // strictly on exprType == Function so ordinary value-position reads (typed as
    // the property value) are unaffected and fall through to the read paths below.
    if (const sema::TypeRef* et = ctx_.exprType(e)) {
        if (et->kind == sema::TypeRef::Kind::Function) {
            if (auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(e))) {
                if (!ps->isIndexer) {
                    const auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e);
                    llvm::Value* recv =
                        (ma && ma->target) ? lowerExpr(*ma->target) : currentThis_;
                    if (llvm::Value* th = makePropertyThunk(ps, recv, ma))
                        return th;
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
            auto savedSemaTy = genericSubstSemaTy_;
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
                                // §14.1 (#768): build the SEMA-type substitution
                                // too — the getter body resolves the member type
                                // (e.g. `_v: T?`) through genericSubstSemaTy_, so
                                // without it a generic nullable field read with
                                // the wrong type (wrong value / mismatched ICmp).
                                std::unordered_map<std::string_view,
                                                   const sema::TypeRef*> newSema;
                                for (std::size_t gi = 0; gi < td->genericParams.size(); ++gi) {
                                    llvm::Type* concrTy = lowerTypeRef(recvTy->genericArgs[gi]);
                                    newSubst[td->genericParams[gi].name] = concrTy;
                                    newSema[td->genericParams[gi].name] =
                                        resolveTypeParamRef(recvTy->genericArgs[gi]);
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
                                genericSubstSemaTy_ = std::move(newSema);
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
            genericSubstSemaTy_ = std::move(savedSemaTy);
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
        // §24.3 (#747): an event read in VALUE position (`var ev := b.Clicked;`,
        // `return Clicked;` — not immediately `.Subscribe`'d) yields the bare
        // bz_event_source_t* (a BORROW of the declaring instance's backing
        // source — the same pointer the direct `.Subscribe` path uses). Without
        // this, fieldAddr's storageMemberOf returns null for an EventSymbol and
        // the IEvent<T> local was loaded with garbage, so its Subscribe never
        // fired. The IEvent<T> value is non-owning (the class disposes the
        // source), so this read takes no ownership.
        if (eventSymbolOf(e))
            if (llvm::Value* src = eventSourcePtr(e))
                return src;
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
            // #720 / §22.13: lazily initialize the owning type on first use.
            if (auto* sym = ctx_.exprSymbol(e)) emitEnsureStaticInit(memberOwner(sym));
            return builder_.CreateLoad(gv->getValueType(), gv);
        }
    }
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        // §24.3 (#747): a BARE event identifier read in VALUE position (e.g.
        // `return Clicked;` returning the event as an IEvent<T>) yields the
        // bz_event_source_t* — the same borrow the `b.Clicked` member-access form
        // produces. Without this, a bare event in a value position lowered as a
        // field load of garbage (IR_FAIL on the method-returned IEvent<T> form).
        if (eventSymbolOf(e))
            if (llvm::Value* src = eventSourcePtr(e))
                return src;
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
            // #720 / §22.13: lazily initialize the owning type on first use.
            if (auto* sym = ctx_.exprSymbol(e)) emitEnsureStaticInit(memberOwner(sym));
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
        // §27.6/§24.4, #880 #881: a bare identifier resolving to an INSTANCE
        // method used in value position binds the implicit `this` receiver,
        // exactly as `this.MethodName` does in the qualified MemberAccessExpr
        // path below. Without this, the env block held a null/garbage receiver
        // — a silent no-op for event handlers (#880) and a SIGSEGV at the
        // call site (#881). Route through makeMethodThunk with currentThis_.
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(e))) {
            if (isStaticMethod(ms)) return makeMethodThunk(ms, nullptr);
            else if (currentThis_) return makeMethodThunk(ms, currentThis_);
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
            // #815 / §9.1: the implementation may be a POSITIONAL record field
            // (which generates a read-only property but is a FieldSymbol, not a
            // PropertySymbol). Spill the record and load the field directly.
            if (sema::FieldSymbol* pfs = classPositionalFieldProperty(
                    concrete->declaration, ps->name)) {
                llvm::Value* obj = recv;
                if (!obj->getType()->isPointerTy()) {
                    llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                    builder_.CreateStore(obj, tmp);
                    obj = tmp;
                }
                llvm::StructType* rst = classStruct(concrete->declaration);
                auto idxIt = fieldIndex_.find(pfs);
                if (rst && idxIt != fieldIndex_.end()) {
                    llvm::Value* slot = builder_.CreateStructGEP(
                        rst, obj, idxIt->second);
                    return builder_.CreateLoad(valTy, slot);
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


// §8.4 (#848): null-safe `=`/`<>` for a record that overloads `operator =`/`<>`,
// when at least one operand is nullable (`T?`) or the `null` literal. Per §8.4,
// `null = null` is `true`, `null = nonNull` is `false`, and both-present invokes
// the equality (here the user operator). The operator (declared on bare `T`) is
// called ONLY on the both-present path — branch-guarded so it never runs on a
// null/zero payload (a record with a reference-typed field would otherwise have
// its comparison dereference a null field). The operator `opm` matches `bin.op`
// (`operator =` for `=`, `operator <>` for `<>`); the not-both-present answer is
// `both-null` for `=` and `exactly-one-null` for `<>`.
llvm::Value* Emitter::lowerNullSafeRecordEquality(const ast::BinaryExpr& bin,
                                                  sema::MethodSymbol* opm) {
    struct Side { llvm::Value* has; llvm::Value* payload; };
    auto eval = [&](const ast::Expr* e) -> Side {
        if (dynamic_cast<const ast::NullLiteralExpr*>(e))
            return {builder_.getFalse(), nullptr};  // payload filled once recTy known
        llvm::Value* v = lowerExpr(*e);
        if (!v) return {nullptr, nullptr};
        const sema::TypeRef* t = ctx_.exprType(*e);
        if (t && isNullableValueTy(t) && v->getType()->isStructTy())
            return {nullablePrimHasValue(v), nullablePrimValue(v)};
        return {builder_.getTrue(), v};  // a bare non-nullable record value
    };
    Side L = eval(bin.left.get());
    Side R = eval(bin.right.get());
    if (!L.has || !R.has) return nullptr;
    llvm::Type* recTy = L.payload ? L.payload->getType()
                      : R.payload ? R.payload->getType() : nullptr;
    if (!recTy) return nullptr;  // both null literals — sema would not resolve opm
    if (!L.payload) L.payload = llvm::Constant::getNullValue(recTy);
    if (!R.payload) R.payload = llvm::Constant::getNullValue(recTy);

    llvm::BasicBlock* entryBB = builder_.GetInsertBlock();
    llvm::Function* fn = entryBB->getParent();
    llvm::Value* bothPresent = builder_.CreateAnd(L.has, R.has, "eq.bothpr");
    auto* callBB  = llvm::BasicBlock::Create(llvm_, "eq.op", fn);
    auto* mergeBB = llvm::BasicBlock::Create(llvm_, "eq.merge", fn);
    builder_.CreateCondBr(bothPresent, callBB, mergeBB);

    builder_.SetInsertPoint(callBB);
    std::vector<llvm::Value*> args{L.payload, R.payload};
    llvm::Value* opres = emitMethodCall(opm, args, /*baseQualified=*/false);
    if (!opres) return nullptr;
    llvm::BasicBlock* callEnd = builder_.GetInsertBlock();  // emitMethodCall may split
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto* present = builder_.CreatePHI(builder_.getInt1Ty(), 2, "eq.present");
    present->addIncoming(builder_.getFalse(), entryBB);
    present->addIncoming(opres, callEnd);
    // Not-both-present answer: `=` → both-null; `<>` → exactly-one-null.
    llvm::Value* tail =
        (bin.op == "=")
            ? builder_.CreateAnd(builder_.CreateNot(L.has),
                                 builder_.CreateNot(R.has), "eq.bothnull")
            : builder_.CreateXor(L.has, R.has, "eq.onenull");
    return builder_.CreateOr(present, tail, "eq.nullsafe");
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
            // §8.4 (#848): a user `operator =`/`<>` on a record is invoked
            // through the null-safe equality wrapper when either operand is a
            // nullable `T?` or the `null` literal — both-null → equal, one-null
            // → not-equal, both-present → the operator on the UNWRAPPED payloads.
            // Without this the `{i8, T}` wrapper (or `ptr null`) was passed
            // straight to the operator (which takes bare `T`) → malformed IR.
            if (bin.op == "=" || bin.op == "<>") {
                auto nullishOperand = [&](const ast::Expr* e) {
                    if (!e) return false;
                    if (dynamic_cast<const ast::NullLiteralExpr*>(e)) return true;
                    const sema::TypeRef* t = ctx_.exprType(*e);
                    return t && isNullableValueTy(t);
                };
                if (nullishOperand(bin.left.get()) ||
                    nullishOperand(bin.right.get()))
                    return lowerNullSafeRecordEquality(bin, opm);
            }
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
                        // #814: widen each narrower payload by ITS OWN signedness.
                        bool lU = bin.left && isUnsignedTargetExpr(*bin.left);
                        bool rU = bin.right && isUnsignedTargetExpr(*bin.right);
                        if (lb < rb)
                            lp = lU ? builder_.CreateZExt(lp, rp->getType())
                                    : builder_.CreateSExt(lp, rp->getType());
                        else
                            rp = rU ? builder_.CreateZExt(rp, lp->getType())
                                    : builder_.CreateSExt(rp, lp->getType());
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
                // Guard: cmp is nullptr when the payload type is neither FP nor integer
                // (e.g. decimal? whose payload is a %bz.decimal struct). A null cmp
                // would crash CreateAnd; when cmp is null fall through to let the
                // non-lifted relational path handle it — decimal relational goes through
                // operator overloads (bz_decimal_cmp) at the user-defined-overload
                // call site above this block, so we never reach here for decimal?.
                if (cmp)
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
                unsigned lb = lp->getType()->getIntegerBitWidth();
                unsigned rb = rp->getType()->getIntegerBitWidth();
                llvm::Type* wide = lb >= rb ? lp->getType() : rp->getType();
                // #814: widen each narrower payload by ITS OWN signedness.
                bool lU = bin.left && isUnsignedTargetExpr(*bin.left);
                bool rU = bin.right && isUnsignedTargetExpr(*bin.right);
                if (lb < rb) lp = lU ? builder_.CreateZExt(lp, wide)
                                     : builder_.CreateSExt(lp, wide);
                if (rb < lb) rp = rU ? builder_.CreateZExt(rp, wide)
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
            // §4.4 / §7.11 / fix #663: decimal? lifted arithmetic — the payload
            // of a decimal? struct is a %bz.decimal (itself a struct), not a scalar.
            // All decimal runtime functions take pointers; mirror the non-nullable
            // path (§6.4) but emit alloca/store/call inside the bothPresent branch.
            // Relational (isRelOp) on decimal? is handled by the early return above.
            llvm::Value* payloadResult = nullptr;
            if (lp->getType()->isStructTy() &&
                lp->getType() == decimalStructType() && isArithOp) {
                llvm::StructType* dst = decimalStructType();
                if (op == "/" || op == "mod") {
                    // §6.4: zero-divisor check inside the bothPresent branch.
                    llvm::Value* lo  = builder_.CreateExtractValue(rp, {0u});
                    llvm::Value* mid = builder_.CreateExtractValue(rp, {1u});
                    llvm::Value* hi  = builder_.CreateExtractValue(rp, {2u});
                    llvm::Value* orv = builder_.CreateOr(
                        builder_.CreateOr(lo, mid), hi);
                    emitGuardThrow(
                        builder_.CreateICmpEQ(orv, llvm::ConstantInt::get(i32(), 0)),
                        "DivideByZeroException");
                }
                if (op == "+" || op == "-" || op == "*" ||
                    op == "/" || op == "mod") {
                    llvm::AllocaInst* aSlot = entryAlloca(dst);
                    llvm::AllocaInst* bSlot = entryAlloca(dst);
                    llvm::AllocaInst* oSlot = entryAlloca(dst);
                    builder_.CreateStore(lp, aSlot);
                    builder_.CreateStore(rp, bSlot);
                    const char* fn_name =
                        op == "+"   ? "bz_decimal_add" :
                        op == "-"   ? "bz_decimal_sub" :
                        op == "*"   ? "bz_decimal_mul" :
                        op == "/"   ? "bz_decimal_div" :
                        /* mod */     "bz_decimal_mod";
                    auto getOVEVtable = [&]() -> llvm::Value* {
                        auto vit = builtinVtable_.find("OverflowException");
                        if (vit == builtinVtable_.end())
                            return llvm::ConstantPointerNull::get(ptrTy());
                        return vit->second;
                    };
                    auto* fn = declareC(fn_name, llvm::Type::getVoidTy(llvm_),
                                        {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                    builder_.CreateCall(fn, {aSlot, bSlot, oSlot, getOVEVtable()});
                    emitPendingCheck();  // per fix #521 / §6.4: runtime sets bz_exc on overflow
                    payloadResult = builder_.CreateLoad(dst, oSlot, "lift.dec.res");
                }
            }
            // Compute the payload result using the same logic as the non-nullable
            // path (including overflow and div-zero checks, per §7.11).
            if (!payloadResult && lp->getType()->isFloatingPointTy()) {
                // FP arithmetic — no overflow/div-zero checks (§6.3).
                if      (op == "+")   payloadResult = builder_.CreateFAdd(lp, rp);
                else if (op == "-")   payloadResult = builder_.CreateFSub(lp, rp);
                else if (op == "*")   payloadResult = builder_.CreateFMul(lp, rp);
                else if (op == "/")   payloadResult = builder_.CreateFDiv(lp, rp);
            } else if (!payloadResult) {
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
                            // Signed INT_MIN op -1 (§6.1): only division overflows
                            // (result not representable); modulo is 0 and must not
                            // raise. srem of INT_MIN,-1 is UB, so force divisor to 1
                            // and select 0 on that path.
                            llvm::Value* minV   = minIntLike(rp);
                            llvm::Value* negOne = llvm::ConstantInt::getSigned(
                                rp->getType(), -1);
                            llvm::Value* isMinNegOne =
                                builder_.CreateAnd(builder_.CreateICmpEQ(lp, minV),
                                                   builder_.CreateICmpEQ(rp, negOne));
                            if (op == "/") {
                                emitGuardThrow(isMinNegOne, "OverflowException");
                                payloadResult = builder_.CreateSDiv(lp, rp);
                            } else {
                                llvm::Value* safeR = builder_.CreateSelect(
                                    isMinNegOne,
                                    llvm::ConstantInt::getSigned(rp->getType(), 1),
                                    rp);
                                llvm::Value* rem = builder_.CreateSRem(lp, safeR);
                                payloadResult = builder_.CreateSelect(
                                    isMinNegOne,
                                    llvm::ConstantInt::get(rp->getType(), 0), rem);
                            }
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
        // fix #663: exclude nullable decimal (decimal?) — those values lower to
        // the {i8, %bz.decimal} tagged struct; the lifted-arithmetic block above
        // handles decimal? operands by extracting the payload before routing here.
        // A decimal? operand reaching this point carries the full wrapper struct,
        // not the bare %bz.decimal that the runtime functions expect.
        bool lDec = lTy && lTy->kind == sema::TypeRef::Kind::Primitive &&
                    lTy->primitive == P::Decimal && !lTy->nullable;
        bool rDec = rTy && rTy->kind == sema::TypeRef::Kind::Primitive &&
                    rTy->primitive == P::Decimal && !rTy->nullable;
        // §5.1: integers implicitly widen to decimal — promote an integer operand.
        // We detect this by: result type is decimal, and one operand is integer.
        const sema::TypeRef* resTy = ctx_.exprType(bin);
        bool resDec = resTy && resTy->kind == sema::TypeRef::Kind::Primitive &&
                      resTy->primitive == P::Decimal && !resTy->nullable;
        // fix #663: if EITHER side is nullable decimal (decimal?), skip this
        // block entirely. The lifted-arithmetic block above handles decimal? op
        // decimal? (both nullable). A decimal? op decimal (mixed nullable vs.
        // non-null) comparison reaches the nullable-value equality path at
        // line ~3989, where the hasValue tag is checked and the inner payload
        // is routed through emitValueEquality → bz_decimal_cmp. Entering here
        // with a nullable side would pass the full {i8, %bz.decimal} wrapper
        // struct to bz_decimal_cmp as if it were a bare %bz.decimal → wrong.
        bool eitherNullableDecimal =
            (lTy && lTy->kind == sema::TypeRef::Kind::Primitive &&
             lTy->primitive == P::Decimal && lTy->nullable) ||
            (rTy && rTy->kind == sema::TypeRef::Kind::Primitive &&
             rTy->primitive == P::Decimal && rTy->nullable);
        if (!eitherNullableDecimal && (lDec || rDec || resDec)) {
            llvm::StructType* dst = decimalStructType();
            // Helper: promote value to decimal struct (alloca + runtime call).
            auto toDecSlot = [&](llvm::Value* v,
                                 const sema::TypeRef* ty) -> llvm::Value* {
                if (ty && ty->kind == sema::TypeRef::Kind::Primitive &&
                    ty->primitive == P::Decimal)
                    return v;  // already decimal struct
                // fix #666: if the LLVM value is already a %bz.decimal struct
                // (e.g. the return value of a LINQ Sum/Average selector lambda
                // whose sema type was not annotated as decimal by the type
                // checker), return it directly.  Without this guard the code
                // below would try CreateSExt(%bz.decimal, i32) — invalid IR.
                if (v->getType() == dst)
                    return v;
                // Integer widening to decimal (§5.1).
                llvm::AllocaInst* slot = entryAlloca(dst);
                if (ty && ty->kind == sema::TypeRef::Kind::Primitive) {
                    // #743: honour unsigned sources (ulong/uint above the signed
                    // max) — emitIntToDecimal picks the correct conversion.
                    emitIntToDecimal(v, ty->primitive, slot);
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
                // per fix #521 / §6.4: pass OverflowException vtable so the
                // runtime can set bz_exc on overflow; emitPendingCheck() unwinds.
                auto getOVEVtable = [&]() -> llvm::Value* {
                    auto vit = builtinVtable_.find("OverflowException");
                    if (vit == builtinVtable_.end())
                        return llvm::ConstantPointerNull::get(ptrTy());
                    return vit->second;
                };
                auto* fn = declareC(fn_name, llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {aSlot, bSlot, oSlot, getOVEVtable()});
                emitPendingCheck();  // per fix #521 / §6.4: runtime sets bz_exc on overflow
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
        // #814: widen each narrower operand by ITS OWN signedness (an unsigned
        // operand zero-extends, a signed operand sign-extends), independent of
        // the operation's common-type signedness (`uns`).
        bool lU = bin.left && isUnsignedTargetExpr(*bin.left);
        bool rU = bin.right && isUnsignedTargetExpr(*bin.right);
        if (lw < rw) l = lU ? builder_.CreateZExt(l, wide)
                            : builder_.CreateSExt(l, wide);
        if (rw < lw) r = rU ? builder_.CreateZExt(r, wide)
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
            // R33: a nullable TYPE-PARAMETER field (`T?`) lowers to the
            // nullable-value struct {i8 hasValue; <T> value} even when T
            // instantiates to a class; isNullableValueTy is false for the
            // unresolved param, so the above misses it and `T? = null` would
            // fall through to struct-equality with a null record type — crashing
            // with the LLVM checkGEPType assertion.  Detect the nullable-value
            // struct by its LLVM shape (element 0 is the hasValue tag) so the
            // null comparison lowers to the hasValue check.
            if (!nvVal) {
                llvm::Value* cand =
                    (bin.left &&
                     !dynamic_cast<const ast::NullLiteralExpr*>(bin.left.get())) ? l
                  : (bin.right &&
                     !dynamic_cast<const ast::NullLiteralExpr*>(bin.right.get())) ? r
                  : nullptr;
                if (cand && cand->getType()->isStructTy()) {
                    auto* st = llvm::cast<llvm::StructType>(cand->getType());
                    if (st->getNumElements() >= 1 &&
                        st->getElementType(0) == nullablePrimHasValTy())
                        nvVal = cand;
                }
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
            llvm::Value* cmpResult;
            if (op == "=") cmpResult = emitStringEq(l, r);
            else           cmpResult = builder_.CreateNot(emitStringEq(l, r));
            // #429 (OPERATOR-OPERAND category) / §4.6 / §28.9: a +1 string
            // producer (concat / interpolation / range-slice / primitive-ToString /
            // user-method-return) fed as a COMPARISON OPERAND is consumed by the
            // comparison (= / <>) but not by any concat chain — nothing else holds
            // its reference.  Release it after emitStringEq has produced the
            // boolean result.  Both sides may be producers independently.
            if (bin.left  && isPlusOneStringProducer(bin.left.get()))
                emitStringRelease(l);
            if (bin.right && isPlusOneStringProducer(bin.right.get()))
                emitStringRelease(r);
            return cmpResult;
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
                llvm::Value* ordResult;
                if (op == "<")       ordResult = builder_.CreateICmpSLT(cmp, zero);
                else if (op == ">")  ordResult = builder_.CreateICmpSGT(cmp, zero);
                else if (op == "<=") ordResult = builder_.CreateICmpSLE(cmp, zero);
                else                 ordResult = builder_.CreateICmpSGE(cmp, zero);
                // #429 (OPERATOR-OPERAND category) / §4.6 / §28.9: release +1
                // string producer operands after the ordinal comparison, mirroring
                // the equality-operator release above.
                if (bin.left  && isPlusOneStringProducer(bin.left.get()))
                    emitStringRelease(l);
                if (bin.right && isPlusOneStringProducer(bin.right.get()))
                    emitStringRelease(r);
                return ordResult;
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
            // Signed INT_MIN op -1 (§6.1): division's mathematical result
            // (-INT_MIN) is not representable in the result type → raise
            // OverflowException. Modulo's mathematical result is 0, which IS
            // representable, so `mod` must NOT raise. But LLVM `srem` of
            // INT_MIN,-1 is undefined behaviour (LangRef) and x86 IDIV faults
            // (SIGFPE), so on that one path force the divisor to 1 and select 0.
            llvm::Value* minV = minIntLike(r);
            llvm::Value* negOne =
                llvm::ConstantInt::getSigned(r->getType(), -1);
            llvm::Value* isMinNegOne =
                builder_.CreateAnd(builder_.CreateICmpEQ(l, minV),
                                   builder_.CreateICmpEQ(r, negOne));
            if (op == "/") {
                emitGuardThrow(isMinNegOne, "OverflowException");
                return builder_.CreateSDiv(l, r);
            }
            llvm::Value* safeR = builder_.CreateSelect(
                isMinNegOne, llvm::ConstantInt::getSigned(r->getType(), 1), r);
            llvm::Value* rem = builder_.CreateSRem(l, safeR);
            return builder_.CreateSelect(
                isMinNegOne, llvm::ConstantInt::get(r->getType(), 0), rem);
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
            // #814 / §6.5: `>>` fill is logical (LShr) iff the SHIFTED value (the
            // LEFT operand) is unsigned; arithmetic (AShr) for a signed value.
            // The shift count's signedness is irrelevant.
            bool lShiftUns = bin.left && isUnsignedTargetExpr(*bin.left);
            return lShiftUns ? builder_.CreateLShr(l, cnt)
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
    // §14.1 / #850: also fires inside a monomorphized generic body where the
    // LHS has sema-type `T?` (a bare nullable type-param, Named/no-declaration),
    // and T resolves under genericSubstSemaTy_ to a concrete value type (e.g.
    // int). The LLVM value is already the {i8, T} struct (emitGenericSpecialization
    // wraps the argument), but isNullableValueTy returned false for the bare T?
    // TypeRef — falling through to the pointer null-check branch, which is wrong
    // for a non-pointer struct. Resolve the concrete nullable here too.
    if (!l->getType()->isPointerTy() && bin.left) {
        if (auto* lt = ctx_.exprType(*bin.left)) {
            // Resolve T? → its concrete nullable form when T is a bound value-type
            // generic parameter (mirrors emitGenericSpecialization lines 10214-10219).
            sema::TypeRef concBuf;
            const sema::TypeRef* effectiveLt = lt;
            if (!isNullableValueTy(lt) && lt->nullable &&
                lt->kind == sema::TypeRef::Kind::Named && !lt->declaration &&
                nullableTypeParamConcrete(lt, concBuf) &&
                isNullableValueTy(&concBuf)) {
                effectiveLt = &concBuf;
            }
            if (isNullableValueTy(effectiveLt)) {
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

    // §6.1 / §4.2: a negative integer LITERAL (`-123`, `-9223372036854775808L`)
    // is a single compile-time constant, not a runtime negation of a positive
    // one. Fold it directly so (a) it bypasses the checked-negate overflow trap
    // — `-9223372036854775808L` is long.MinValue, not an overflow of +2^63 —
    // and (b) the magnitude 2^63 (not a representable positive long) never has
    // to exist as a runtime value. This mirrors the compile-time fold the
    // enum/const path already performs via sema::foldIntConst. Only a plain
    // int literal operand is folded; `-x` on a variable still runs the checked
    // negate (and still traps on int.MinValue per §6.1).
    if (un.op == "-") {
        if (auto* lit =
                dynamic_cast<const ast::IntLiteralExpr*>(un.operand.get())) {
            llvm::Type* ty = i32();
            const sema::TypeRef* rt = ctx_.exprType(un);
            if (!rt || rt->kind != sema::TypeRef::Kind::Primitive)
                rt = ctx_.exprType(*un.operand);
            if (rt && rt->kind == sema::TypeRef::Kind::Primitive)
                ty = lowerPrimitive(rt->primitive);
            std::uint64_t mag =
                static_cast<std::uint64_t>(parseIntLiteral(lit->text));
            // Two's-complement negation in unsigned arithmetic (well-defined,
            // wraps for the 2^63 / 2^31 boundary instead of overflowing).
            return llvm::ConstantInt::get(
                ty, static_cast<std::uint64_t>(0) - mag, /*isSigned=*/true);
        }
    }

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
        // §6.1: unary negation is checked for overflow.
        if (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(1)) {
            if (isUnsignedTargetExpr(*un.operand)) {
                // #747 R42: on an UNSIGNED type the mathematical result of `-x`
                // is negative, representable only when x == 0; any non-zero
                // value overflows. (Previously this fell through to the signed
                // INT_MIN check and silently wrapped, e.g. -uint(1) →
                // uint.MaxValue instead of OverflowException.)
                emitGuardThrow(
                    builder_.CreateICmpNE(
                        v, llvm::ConstantInt::get(v->getType(), 0)),
                    "OverflowException");
            } else {
                // Signed: overflows only at INT_MIN (-INT_MIN is not
                // representable).
                emitGuardThrow(builder_.CreateICmpEQ(v, minIntLike(v)),
                               "OverflowException");
            }
        }
        return builder_.CreateNeg(v);
    }
    if (op == "~") return builder_.CreateNot(v);   // bitwise complement
    if (op == "+") return v;                       // identity
    if (op == "not") return builder_.CreateNot(v); // i1 logical negation
    // §28.3: `move x` is a compile-time ownership transfer.  At runtime it
    // yields the operand value unchanged (the same reference).  For a `move`
    // of a `using var` LOCAL, null the source alloca slot immediately so the
    // scope-end disposal (emitDisposals) finds null and skips — preventing a
    // double-dispose when the moved-out object is freed by its new owner's
    // Dispose cascade (e.g. `using var b; using var a := new T(move b)`).
    // This mirrors the collection-slot case handled by lowerMoveFromCollectionSlot.
    // NOTE: plain `var` locals (isUsing=false) are NOT nulled here: the
    // programmer may borrow from them after the move (e.g. `new T(move n, n)`),
    // and plain-var locals are not registered for scope-end disposal.
    if (op == "move") {
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(un.operand.get())) {
            sema::Symbol* sym = ctx_.exprSymbol(*id);
            if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
                if (loc->isUsing) {
                    if (llvm::AllocaInst* slotAI = localSlot(*un.operand)) {
                        llvm::Type* ety = nullptr;
                        llvm::Value* addr = boxedStorage(slotAI, ety);
                        if (ety && ety->isPointerTy()) {
                            // Only null pointer-typed slots (class/interface refs).
                            // Value-type using-var locals have no owned disposal.
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), addr);
                        }
                    }
                }
            } else if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym)) {
                // #818 / §28.8.1: `move _field` (an UNQUALIFIED owned-field move —
                // the §6/Guide-1 detach idiom, e.g. `return move _log`) transfers
                // the field's object out. Null the field slot so the owner's
                // destructor — whose owned-field disposal is null-guarded
                // (emitOwnedFieldDisposals) — skips it instead of disposing the
                // already-transferred object again (a double-free → SIGSEGV).
                // Mirrors the local using-var null above and the collection-slot
                // move (lowerMoveFromCollectionSlot). Only pointer-typed
                // (class/interface) owned fields carry a disposal obligation.
                //
                // §28.8.1, #899: NOT inside a destructor body
                // (emittingDisposerBody_). The same reasoning as for the qualified
                // `move this._f` form (see the MemberAccessExpr branch below, fixed
                // in #897/#898): inside a destructor the SOLE disposal site for an
                // owned field is the auto-dispose pass (emitOwnedFieldDisposals),
                // which null-guards the load before disposing. Nulling the field
                // slot here (before that pass runs) makes it skip the still-owned
                // object → a LEAK. The using-var scope-end disposal is also
                // suppressed in a destructor body (emitDispose checks
                // emittingDisposerBody_), so no disposal site remains → leak.
                // Guard the null-store on !emittingDisposerBody_, mirroring the
                // qualified-form fix, so inside a destructor the field is left
                // non-null and the auto-dispose pass disposes it exactly once.
                if (isOwnedReferenceField(fs) && !emittingDisposerBody_) {
                    llvm::Type* fty = nullptr;
                    if (llvm::Value* addr = implicitFieldAddr(*un.operand, fty)) {
                        if (fty && fty->isPointerTy())
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), addr);
                    }
                }
            }
        } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                       un.operand.get())) {
            // §28.8.1, #897 #898 (Part B): the QUALIFIED `move this._field`
            // form. Part A (ownership pass) now rejects a SECOND move of an
            // already-moved-from field, so a sound program never double-owns;
            // but to make the qualified move consistent with the bare `move
            // _field` above (form-parity, #825) and remove the use-after-free
            // even if a path evades Part A, null the field slot after the move
            // so a subsequent read sees null, not the still-populated original
            // pointer. The owner's destructor owned-field disposal is null-
            // guarded (emitOwnedFieldDisposals), so nulling here makes it skip
            // the already-transferred object instead of double-freeing it.
            //
            // Only the `this._field` form is nulled. Compute the slot via
            // currentThis_ + the field index — the same lightweight StructGEP
            // implicitFieldAddr uses for the bare form — NOT lvalueAddr/
            // fieldAddr, whose re-lowering of the `this` receiver and generic-
            // substitution side effects produced malformed IR for the canonical
            // destructor `using var u := move this._inner;` case (it re-emitted
            // the receiver after the field value `v` had already been read).
            //
            // NOT inside a destructor body (emittingDisposerBody_): per the
            // #383/#763 design, a `move this._f` in a destructor does NOT get its
            // own scope-end disposal — emitDispose suppresses it — and the
            // SOLE disposal site is the owned-field auto-dispose pass
            // (emitOwnedFieldDisposals), which loads the field and disposes it.
            // Nulling the field here would make that pass skip the still-owned
            // object → a LEAK (destructor_field_move_runtime_763). The Part A
            // sema check already rejects a double-move in a destructor (so the
            // UAF this null guards against cannot occur there), so skipping the
            // null in the disposer body is both necessary and sound.
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    ma->target.get());
                tid && tid->name == "this" && currentThis_ &&
                !emittingDisposerBody_) {
                if (auto* fs = dynamic_cast<sema::FieldSymbol*>(
                        ctx_.exprSymbol(*ma))) {
                    auto idxIt = fieldIndex_.find(fs);
                    if (isOwnedReferenceField(fs) && fs->parent &&
                        idxIt != fieldIndex_.end()) {
                        llvm::StructType* st = classStruct(fs->parent);
                        if (st && st->getElementType(idxIt->second)
                                      ->isPointerTy()) {
                            llvm::Value* addr = builder_.CreateStructGEP(
                                st, currentThis_, idxIt->second);
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), addr);
                        }
                    }
                }
            }
        }
        return v;
    }
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
            // fix #747 R43 / §27.1: a `ref`/`out` lambda parameter is passed by
            // POINTER — the argument is the caller's storage address, exactly as
            // a named method's `ref n` parameter (§22.3). Without this the lambda
            // received the value by copy, so a mutation in the body never reached
            // the caller's variable (the write-back was silently lost). The body
            // binding below registers it in refParams_ so reads/writes route
            // through the incoming pointer.
            if (par && isByRefMod(par->modifier)) ty = ptrTy();
            userParamTys.push_back(ty);
            ++pi;
        }
    }

    // Pre-determine return type from sema's exprType of the body.
    // retSemaTy tracks the sema TypeRef that SET retTy (used below for
    // nullable-value promotion in the expr-body emit path — fix #541 / §7).
    llvm::Type* retTy = i32();
    const sema::TypeRef* retSemaTy = nullptr;
    // #800 Layer-2 / §30.6: an ASYNC expression-bodied lambda's function type is
    // `() -> Task<T>` (sema wraps the inferred return), and the async state machine
    // wraps the body's natural value T into that outer Task. Derive retTy/retSemaTy
    // from the wrapped funcReturn (Task<T>), exactly as the block-body path below
    // does — NOT from the raw body element type. Using the raw element type left
    // the async result-element typing inconsistent with the block case (the
    // no-annotation fallback's `retTy != i32()` gate then dropped an int element →
    // null result slot → SIGSEGV).
    if (lam.exprBody && lam.isAsync) {
        if (sema::TypeRef* lamTy = ctx_.exprType(lam)) {
            if (lamTy->kind == sema::TypeRef::Kind::Function && lamTy->funcReturn) {
                retTy = lowerTypeRef(lamTy->funcReturn);
                retSemaTy = lamTy->funcReturn;
            }
        }
    } else if (lam.exprBody) {
        if (sema::TypeRef* rt = ctx_.exprType(*lam.exprBody)) {
            retTy = lowerTypeRef(rt);
            retSemaTy = rt;
        } else {
            // fix #678 (narrowed) / §27.2: sema leaves the body expression
            // untyped (null exprType) for two reasons:
            //   A. A void intrinsic call (Console.Write/WriteLine) — codegen's
            //      lowerExpr returns nullptr for it, so the lambda body would
            //      emit `ret void` into an i32-returning function → IR verifier
            //      failure.  Infer void by recognising the Console pattern.
            //   B. An unresolved value expression (unannotated IIFE `(x)=>x*2`,
            //      generic-closure body `(x)=>x.v`, etc.) — null here means
            //      "type unknown, will be resolved via expectedFunctionType_ or
            //      left at i32 fallback".  These cases are NOT void; leave retTy.
            //
            // User-defined void methods (md->returnType==null) get a Kind::Unit
            // exprType from sema (ExprTypecheck_Calls.cpp §549-553); lowerTypeRef
            // maps Unit→voidTy and the branch above (ctx_.exprType != null) fires
            // for them — they are already handled without reaching this else.
            //
            // The null-funcReturn of the lambda's own sema TypeRef is NOT a
            // reliable void indicator: iife/generic-closure lambdas also have
            // funcReturn==null when the body type is unresolvable at sema time
            // (issue #678 rejection — the broader check broke 18 such tests).
            if (isLambdaExprBodyVoidBuiltin(lam))
                retTy = llvm::Type::getVoidTy(llvm_);
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
        // #429 / #672 / §27.5 / §28.9: resolved sema type for retain/release
        // classification in the per-lambda envdtor.  Non-null for params/locals
        // where type resolution succeeded; null for `this` captures and when
        // type is unavailable.  Drives the envdtor's retain/release decisions:
        //   - string (Primitive::String): bz_string_retain at capture + release in envdtor
        //   - Function: bz_closure_retain at capture + release in envdtor
        //   - other pointer types (class borrow, value): cell freed only — per
        //     §28.9/§28.3, class captures are borrows; no retain/release on the
        //     captured object itself.
        const sema::TypeRef*   semaTy = nullptr;
        // §27.5 / §28.9 / #672: true when this capture is a +1-box-producer local
        // captured by exactly ONE lambda env (boxCaptureLocals_) — the
        // envdtor performs the DEFERRED scope-end bz_box_release that
        // BoxEscapeWalker suppressed at the capture. Disjoint from the string/
        // closure release branches (a box has an Object static type, never
        // String/Function).
        bool                   releaseBoxInEnvdtor = false;
        // §27.5 / §28.9 / #694: true when this capture's boxed cell is held by
        // exactly ONE env (singleCaptureLocals_) whose lambda DEFINITELY escapes
        // (returned — definitelyEscapingLambdas_). The escaped closure's envdtor
        // frees the 8-byte capture cell at last-drop. Single holder ⇒ no
        // double-free; definite escape ⇒ outer scope gone ⇒ no UAF.
        bool                   freeCellInEnvdtor = false;
    };
    std::vector<CaptureEntry> captures;

    // Save state (needed for capture slot access and restoration).
    auto savedLambdaParamHints = lambdaParamTypeHint_;  // restore after body
    // #731: this lambda's own retTy/params were just derived from
    // expectedFunctionType_. If the body is itself a lambda (a nested
    // higher-order lambda, `(x) => (y) => x` under a `(int)->(int)->int`
    // annotation), the INNER lambda must see ITS expected type — the outer's
    // funcReturn — not the outer's full type. Descend before body emission so a
    // nested body-lambda derives the right return type; otherwise the inner
    // lambda's body was typed against the wrong expected type → `ret i32` vs ptr.
    const sema::TypeRef* savedExpectedFT = expectedFunctionType_;
    if (expectedFunctionType_ && expectedFunctionType_->funcReturn &&
        expectedFunctionType_->funcReturn->kind == sema::TypeRef::Kind::Function)
        expectedFunctionType_ = expectedFunctionType_->funcReturn;
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

    // #429 / §27.5: record freshly-boxed cells for this lambda so lowerLocalDecl
    // can register boxedCellFree disposals when the lambda is non-escaping.
    // cellFreeEligible_ (computed by the pre-scan collectCellFreeEligible before
    // lowering) says which captured locals/params are captured ONLY by non-
    // escaping lambdas and are thus safe to free at scope exit. A cell is also
    // "freshly boxed" when boxLocalSlot allocates a new ptr-alloca for it (vs
    // the Already-boxed path where another lambda already controls the alloca
    // — in that case this is the SECOND capture; the cell was created by the
    // first lambda's compilation and its eligibility was already determined then).
    // For-var loop captures use a fresh cell EVERY iteration so freeing only at
    // scope exit would miss prior iterations; those are excluded (documented).
    lastLambdaCapturedCells_.clear();

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
        bool alreadyBoxed = cellVars_.count(slot) != 0;
        CaptureEntry ce;
        ce.valTy = alreadyBoxed ? cellVars_[slot]
                                : slot->getAllocatedType();
        llvm::AllocaInst* newSlot = nullptr;
        bool wasGuardedParam = false;
        ce.envValue = boxLocalSlot(slot, newSlot, /*forLoopVar=*/false,
                                   &wasGuardedParam);  // the shared cell pointer
        paramSlots_[sym] = newSlot;  // rebind to the ptr cell-holder
        rebindDisposalSlot(slot, newSlot);
        ce.sym = sym;
        ce.decl = nullptr;
        // #429: record freshly-boxed param cells that are eligible for free
        // at scope exit (cellFreeEligible_ pre-scan says the param is captured
        // only by non-escaping lambdas; params are never for-var).
        // Skip guarded-boxing cells — a param declared outside a loop whose
        // lambda capture is inside the loop body must not have its cell freed
        // at inner-scope exit (the cell is reused across iterations).
        if (!alreadyBoxed && !wasGuardedParam &&
            currentFn_ == cellFreeEligibleFn_ &&
            cellFreeEligible_.count(static_cast<const void*>(sym))) {
            lastLambdaCapturedCells_.push_back({newSlot,
                                                static_cast<const void*>(sym)});
        }
        // #429/#672: resolve sema type for retain/release classification.
        if (auto* par = dynamic_cast<const sema::ParameterSymbol*>(sym)) {
            if (par->resolvedDeclType)
                ce.semaTy = par->resolvedDeclType;
            else if (par->declType)
                ce.semaTy = ctx_.resolvedType(*par->declType);
        }
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
        bool alreadyBoxed = cellVars_.count(slot) != 0;
        CaptureEntry ce;
        ce.valTy = alreadyBoxed ? cellVars_[slot]
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
        // #806 / #782: a captured move-only OWNING inline-array
        // ([N owned]T? / [N owned]T) is initialised field-by-field (a per-slot
        // GEP store), so boxLocalSlot's whole-slot-store freshness heuristic does
        // not fire and it would use GUARDED boxing — one cell allocated on the
        // first pass and reused on every loop iteration. That is why an in-loop
        // capture leaks today: the #782 element-dispose-through-cell + cell-free
        // at the lambda scope is only sound for a FRESH per-iteration cell (a
        // reused guarded cell would be disposed+freed on iteration 1 then
        // dereferenced on iteration 2 → UAF), so the in-loop case is excluded
        // (loopStack non-empty, see guardedOk below) and the elements never get
        // disposed. Force FRESH boxing for an inline owned array captured inside a
        // loop so each iteration owns its own cell — the element-dispose then
        // fires per iteration with no cross-iteration corruption. The non-loop
        // case keeps the proven guarded path (loopStack empty ⇒ allocated once),
        // and non-array captured aggregates keep the guarded default entirely.
        bool isInlineOwnedArray = false;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(decl)) {
            const sema::TypeRef* rt =
                ld->type ? ctx_.resolvedType(*ld->type)
                         : (ld->initializer ? ctx_.exprType(*ld->initializer)
                                            : nullptr);
            if (rt && rt->kind == sema::TypeRef::Kind::InlineArray &&
                rt->arrayOwned)
                isInlineOwnedArray = true;
        }
        bool forceFresh = isForVar ||
                          (isInlineOwnedArray && !loopStack_.empty());
        bool wasGuardedLocal = false;
        ce.envValue = boxLocalSlot(slot, newSlot, /*forLoopVar=*/forceFresh,
                                   &wasGuardedLocal);
        locals_[decl] = newSlot;  // rebind to the ptr cell-holder
        rebindDisposalSlot(slot, newSlot);
        ce.sym = nullptr;
        ce.decl = decl;
        // #429: record freshly-boxed, non-for-var local cells that are
        // eligible for free at scope exit (cellFreeEligible_ pre-scan says the
        // local is captured only by non-escaping lambdas).
        // Skip guarded-boxing cells: when a local is declared in an outer scope
        // (e.g. method body) and captured by a lambda in an inner scope (e.g.
        // loop body), boxLocalSlot uses the guarded path — a single cell is
        // allocated once and reused across loop iterations.  Freeing it at the
        // inner-scope (lambda-declaration) exit would corrupt subsequent
        // iterations.  Such cells continue to be freed at the outer scope exit
        // through the variable's own disposal entry (future work).
        if (!alreadyBoxed && !isForVar &&
            currentFn_ == cellFreeEligibleFn_ &&
            cellFreeEligible_.count(static_cast<const void*>(decl))) {
            // #782: if the captured local is a move-only OWNING inline-array
            // ([N owned]T? / [N owned]T), record N so lowerLocalDecl registers an
            // element-dispose-through-cell next to the boxedCellFree — otherwise
            // the owned elements (boxed into the cell) leak. ce.valTy is the
            // pre-boxing [N x ptr] alloca type (this branch requires !alreadyBoxed,
            // so it was set from slot->getAllocatedType()).
            unsigned inlineN = 0;
            if (auto* ld = dynamic_cast<const ast::LocalDecl*>(decl)) {
                const sema::TypeRef* rt =
                    ld->type ? ctx_.resolvedType(*ld->type)
                             : (ld->initializer ? ctx_.exprType(*ld->initializer)
                                                : nullptr);
                if (rt && rt->kind == sema::TypeRef::Kind::InlineArray &&
                    rt->arrayOwned)
                    if (auto* at = llvm::dyn_cast<llvm::ArrayType>(ce.valTy))
                        inlineN = static_cast<unsigned>(at->getNumElements());
            }
            // #429 / #782: guarded-boxing cells are normally excluded — a local
            // declared in an outer scope and captured by an inner-LOOP lambda
            // reuses one cell across iterations, so freeing it at the lambda
            // scope would corrupt later iterations. A move-only inline-owned-array
            // uses guarded boxing too (its per-slot init is not a whole-array
            // store, so boxLocalSlot's freshEachIteration heuristic is false), but
            // when the capture is NOT lexically inside any loop (loopStack_ empty)
            // the guarded cell is allocated exactly once — so disposing + freeing
            // it at scope end is sound. Non-array guarded cells keep the
            // conservative exclusion (a safe leak).
            bool guardedOk = !wasGuardedLocal ||
                             (inlineN > 0 && loopStack_.empty());
            if (guardedOk)
                lastLambdaCapturedCells_.push_back({newSlot,
                                                    static_cast<const void*>(decl),
                                                    inlineN});
        }
        // #429/#672: resolve sema type for retain/release classification.
        for (const sema::Symbol* s : capturedSyms) {
            if (auto* loc = dynamic_cast<const sema::LocalSymbol*>(s)) {
                if (loc->decl == decl) {
                    ce.semaTy = loc->declType;
                    break;
                }
            }
        }
        // §27.5 / §28.9 / #672 / #802: arm the envdtor box release for a
        // +1-box-producer local captured by a closure. Every capturing env's
        // envdtor releases one box reference; the rc stays balanced via the
        // rc-retain model — the FIRST capture transfers the producer's +1 (its
        // scope-end release was suppressed by BoxEscapeWalker), and every
        // ADDITIONAL (alreadyBoxed) capture bz_box_retains an independent +1 here.
        // So N envs ⇒ rc = 1 (producer) + (N-1) (retains) = N, and N envdtor
        // releases bring it to 0 after the last env dies — no leak, no double-free.
        // Exclude guarded-boxing (a box producer in an outer scope captured by an
        // inner-LOOP lambda reuses ONE cell across iterations; the retain/release
        // bookkeeping would not match the per-iteration boxing — leak instead).
        if (!wasGuardedLocal &&
            currentFn_ == boxCaptureLocalsFn_ &&
            boxCaptureLocals_.count(static_cast<const void*>(decl))) {
            ce.releaseBoxInEnvdtor = true;
            if (alreadyBoxed) {
                // 2nd+ capturing env: retain so this env owns an independent +1 to
                // release in its envdtor. ce.envValue is the shared cell pointer;
                // the box pointer is its loaded contents.
                llvm::Value* box =
                    builder_.CreateLoad(ptrTy(), ce.envValue, "box.recap");
                emitBoxRetain(box);
            }
        }
        // #694: free the 8-byte capture cell in the envdtor when this local is
        // captured by exactly ONE env (singleCaptureLocals_) and THIS lambda
        // DEFINITELY escapes (definitelyEscapingLambdas_ — directly returned). The
        // scope-end boxedCellFree does not fire for an escaping closure, so the
        // sole env's envdtor is the cell's only free (no double-free); the defining
        // scope is gone before the escaped closure dies (no UAF). Exclude
        // guarded/for-var cells (shared/per-iteration holders).
        if (!alreadyBoxed && !wasGuardedLocal && !isForVar &&
            currentFn_ == boxCaptureLocalsFn_ &&
            definitelyEscapingLambdas_.count(static_cast<const void*>(&lam)) &&
            singleCaptureLocals_.count(static_cast<const void*>(decl)))
            ce.freeCellInEnvdtor = true;
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
    auto savedCellVarSyncSlot = cellVarSyncSlot_;
    auto savedDisposeStack = disposeStack_;
    // #429 / §27.5: save/restore lastLambdaCapturedCells_ across body emission
    // so nested lambdas compiled inside this body do not clobber the cells
    // recorded by THIS lambda's capture-boxing phase above.  The outer lambda's
    // cells are already in lastLambdaCapturedCells_; save them now, let the
    // body run with a fresh vector, and restore them before returning so
    // lowerLocalDecl (our caller) sees the outer lambda's cells.
    auto savedLambdaCapturedCells = std::move(lastLambdaCapturedCells_);
    lastLambdaCapturedCells_.clear();
    currentFn_ = fn;
    currentThis_ = nullptr;
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    cellVars_.clear();
    cellVarSyncSlot_.clear();
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
            sema::Symbol* psym = (*paramSyms)[idx];
            auto* par = dynamic_cast<sema::ParameterSymbol*>(psym);
            // fix #747 R43 / §27.1: a `ref`/`out` lambda parameter receives the
            // caller's storage ADDRESS (a pointer, per the param-type override
            // above). Register it in refParams_ so body reads/writes route
            // through that pointer (lvalueAddr → refParamAddr), mirroring the
            // named-method ref ABI — this is what writes the mutation back.
            if (par && isByRefMod(par->modifier)) {
                llvm::Type* elemTy =
                    par->declType ? lowerType(par->declType) : i32();
                refParams_[psym] = {&*it, elemTy};
                continue;
            }
            auto* slot = entryAlloca(it->getType());
            builder_.CreateStore(&*it, slot);
            paramSlots_[psym] = slot;
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
                // #429 (closure CALL-RETURN category) / §28.9 / §27.5: a lambda
                // with an expression body must also mint +1 for reference-counted
                // return types, exactly as the block-body path does via
                // ReturnStmt lowering (LlvmBackend_Stmts.cpp lines ~718-751).
                // Without this, a `() => <borrow>` lambda body returns a +0
                // borrow to its caller, who then tries to release a +1 that was
                // never minted — double-free (PRIME DIRECTIVE violation).
                // Mirror ReturnStmt: TRANSFER (skip retain) when the expr already
                // owns a +1; RETAIN (mint) when it is a borrow.
                //
                // String: retain unless the body is a +1 producer (concat/interp/
                //   range-slice/numeric-ToString/user-method-call) or is an
                //   identifier naming a non-nullable local that owns +1.
                if (v && v->getType()->isPointerTy() &&
                    retSemaTy && !retSemaTy->nullable &&
                    retSemaTy->kind == sema::TypeRef::Kind::Primitive &&
                    retSemaTy->primitive == sema::PrimitiveType::String &&
                    !isPlusOneStringProducer(lam.exprBody.get()) &&
                    !returnedStringLocalOwnsPlusOne(lam.exprBody.get()))
                    emitStringRetain(v);
                // Closure: retain unless the body is a +1 closure producer
                //   (lambda / method-ref / user-method-closure-return) or is an
                //   identifier naming a closure local that owns +1.
                if (v && v->getType()->isPointerTy() &&
                    retSemaTy &&
                    retSemaTy->kind == sema::TypeRef::Kind::Function &&
                    !isPlusOneClosureProducer(lam.exprBody.get()) &&
                    !isUserMethodClosureReturnCall(lam.exprBody.get()) &&
                    !returnedClosureLocalOwnsPlusOne(lam.exprBody.get()))
                    emitClosureRetain(v);
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
    expectedFunctionType_ = savedExpectedFT;  // #731
    lambdaParamTypeHint_ = std::move(savedLambdaParamHints);
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    refParams_ = std::move(savedRefParams);
    cellVars_ = std::move(savedCellVars);
    cellVarSyncSlot_ = std::move(savedCellVarSyncSlot);
    disposeStack_ = std::move(savedDisposeStack);
    // #429: restore the outer lambda's captured cells (see save above).
    lastLambdaCapturedCells_ = std::move(savedLambdaCapturedCells);
    // (The inner function's lastLambdaCapturedCells_ — populated during body
    // emission — is discarded here because it belongs to lambdas defined inside
    // this lambda's body, not to this lambda's own captures.)
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);  // #546 / §31.2: restore
    unwindTarget_ = savedUnwind;
    propagateBB_ = savedProp;
    currentCaughtExc_ = savedCaught;
    unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    if (savedBB) builder_.SetInsertPoint(savedBB);

    // --- Build the closure struct { fn_ptr, env_ptr, rc, envdtor } ---
    // #422 / #672 / §27.5 / §28.9: allocate a heap closure cell { ptr fn,
    // ptr env, i64 rc, ptr envdtor } (32 bytes). fn (off 0) and env (off 8)
    // keep their offsets; getTypeAllocSize is the single source of truth for
    // the size so the rc/envdtor stores never overflow.
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
            // #429 / #672 / §27.5 / §28.9: NO extra retain at capture-store time.
            //
            // RATIONALE: per §28.3/§28.9, ALL captures are borrows from the
            // language's ownership perspective. When a string or function-typed local
            // is captured, both StringEscapeWalker and ClosureEscapeWalker mark it
            // "escaped" (FINDING [1]) which SUPPRESSES the scope-end release that
            // would otherwise fire. That suppressed release is the ONLY extra
            // reference: the reference already in the local's slot (either the
            // producer's +1 or a borrow-retain's +1) is exactly the one the env
            // cell should eventually release via the envdtor. No NEW reference is
            // created at capture-store time; the existing +1 is "transferred" from
            // the local slot to the env-cell's lifetime when the outer scope ends.
            // Emitting an extra retain here would inflate the RC to +2 (outer-scope
            // release suppressed → net +2 → envdtor releases once → leaks at +1).
            // The PRIME DIRECTIVE (over-release worse than leak) is satisfied:
            // the envdtor is the sole release of the existing reference.
            // Class/value captures: NO RC obligation on the closure side; env holds
            // a raw pointer borrow with no retain/release. §28.9 §28.3.
        }
    }
    // Store env_ptr at offset sizeof(ptr).
    llvm::Value* envSlot = builder_.CreateGEP(
        ptrTy(), closureObj, {builder_.getInt32(1)});
    builder_.CreateStore(envAlloc, envSlot);
    initClosureRc(closureObj);     // #422: producer's reference, rc := 1

    // #429 / #672 / §27.5 / §28.9: emit per-lambda envdtor if any non-`this`
    // capture exists.  The envdtor runs at last-drop (from bz_closure_release)
    // to release the string/closure references that were owned by the captured
    // locals and whose scope-end releases were suppressed (FINDING [1]: escape
    // detection prevents the outer-scope release when a local is captured).
    // The envdtor acts as the DEFERRED scope-end release for those references.
    // `this` captures (direct receiver ptr, no heap cell) need no cleanup.
    // methodThunk closures keep null envdtor — their env contains only `this`.
    {
        bool hasNonThisCapture = false;
        for (const auto& ce : captures)
            if (!ce.isThis) { hasNonThisCapture = true; break; }

        if (!hasNonThisCapture || envFieldTys.empty()) {
            initClosureEnvdtor(closureObj); // null — no env cells to free
        } else {
            // Emit the envdtor function alongside the trampoline.
            llvm::BasicBlock* preDtorBB = builder_.GetInsertBlock();

            std::string dtorName =
                "bz$lam$" + std::to_string(lamId) + "$envdtor";
            auto* dtorTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
            auto* dtorFn = llvm::Function::Create(
                dtorTy, llvm::Function::InternalLinkage,
                dtorName, module_.get());

            auto* dtorEntry =
                llvm::BasicBlock::Create(llvm_, "entry", dtorFn);
            builder_.SetInsertPoint(dtorEntry);

            llvm::Value* envArg = &*dtorFn->args().begin();
            auto* envTyD = llvm::StructType::get(llvm_, envFieldTys);

            for (std::size_t i = 0; i < captures.size(); ++i) {
                const CaptureEntry& ce = captures[i];
                if (ce.isThis) continue;
                // Load cell pointer from env slot.
                llvm::Value* fld = builder_.CreateStructGEP(
                    envTyD, envArg, static_cast<unsigned>(i));
                llvm::Value* cell =
                    builder_.CreateLoad(ptrTy(), fld, "cell");
                // Release the refcounted value stored in the cell.  This is
                // the DEFERRED scope-end release for string/closure captures:
                // the outer scope's release was suppressed by escape detection
                // (FINDING [1]) so the reference still exists.  PRIME DIRECTIVE:
                // exactly one release per suppressed scope-end (= the reference
                // that was already in the local's slot before the capture).
                // Value-type captures (int/bool/struct): no RC obligation.
                // Class-borrow captures: per §28.9/§28.3 ALL captures are
                // borrows; class instances are NOT ref-counted by the closure.
                // CELL ITSELF: boxed capture cells (the shared heap allocation
                // from boxLocalSlot) are NOT freed by the envdtor.  The cell is
                // shared with the outer scope's ptr-holder alloca (newSlot);
                // freeing it here can UAF the outer scope's access path (e.g.
                // a using-var class disposal loads object ptr through the cell).
                // The cell continues to leak — a pre-existing contained leak,
                // safe in the direction of PRIME DIRECTIVE (never over-releases).
                // Future work: free non-escaping cells at outer-scope exit.
                if (isStringTypeRefNonNull(ce.semaTy)) {
                    // String capture: load and release the +1 we retained.
                    llvm::Value* str =
                        builder_.CreateLoad(ptrTy(), cell, "str");
                    emitStringRelease(str);
                } else if (ce.semaTy &&
                           ce.semaTy->kind == sema::TypeRef::Kind::Function) {
                    // Closure capture: load and release the +1 we retained.
                    llvm::Value* cl =
                        builder_.CreateLoad(ptrTy(), cell, "cl");
                    emitClosureRelease(cl);
                } else if (ce.releaseBoxInEnvdtor) {
                    // §8.6 / §28.9 / #672: boxed-VALUE capture, held by exactly
                    // one env (boxCaptureLocals_). BoxEscapeWalker
                    // suppressed the producer's scope-end bz_box_release because
                    // the local was captured; this envdtor performs that single
                    // deferred release. Mirrors the scope-end boxRelease disposal
                    // (load the box pointer from the cell, bz_box_release). The
                    // box is a value-type leaf so only the wrapper is freed; never
                    // a class instance (a class coerced to Object is not a box
                    // producer, so it never enters boxCaptureLocals_).
                    // bz_box_release is null-safe.
                    llvm::Value* box =
                        builder_.CreateLoad(ptrTy(), cell, "box");
                    emitBoxRelease(box);
                }
                // All other capture types (value, class borrow): no action.
                // §27.5 / #694: free the 8-byte capture CELL itself when this is
                // the sole env of a DEFINITELY-escaping capture (freeCellInEnvdtor).
                // Runs AFTER the content release above (which reads through the
                // cell). The scope-end boxedCellFree did not fire (the closure
                // escaped), and this is the single holder, so this is the cell's
                // only free — no double-free; the defining scope is gone before the
                // escaped closure dies, so no later access through the cell — no UAF.
                if (ce.freeCellInEnvdtor)
                    builder_.CreateCall(freeFn(), {cell});
            }
            builder_.CreateRetVoid();

            // Restore builder to the outer-function BB.
            builder_.SetInsertPoint(preDtorBB);

            // Store envdtor fn ptr at cell field 3 (off 24).
            builder_.CreateStore(
                dtorFn,
                builder_.CreateStructGEP(closureCellType(), closureObj, 3));
        }
    }

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
    // fix #747 R43 / §22.11 / §28.3: during a CLASS object-initializer block's
    // setter sequence, the freshly-`new`'d instance is an UNATTACHED OWNED
    // value. If a setter (or a setter's value expression) throws, §22.11
    // requires the partially-initialized instance to be disposed by the implicit
    // try/finally before the exception propagates — exactly as a plain owned
    // `new T(...)` dropped on an exception path (§28.3). Register an unwindOnly
    // disposal (dynamic dispatch through the object's vtable) that is active
    // ONLY across the setter loop: it fires if a setter unwinds, and is removed
    // once the loop completes (the result is returned and ownership transfers to
    // the binding/caller, whose own discharge takes over — keeping it would
    // double-dispose on a LATER throw). Records are value types with no disposal
    // obligation, so this is the class form only.
    llvm::AllocaInst* initGuardSlot = nullptr;
    std::size_t initGuardIdx = static_cast<std::size_t>(-1);
    if (!isRecord && !disposeStack_.empty()) {
        initGuardSlot = entryAlloca(ptrTy());
        builder_.CreateStore(base, initGuardSlot);
        Disposal g{initGuardSlot, nullptr};
        g.unwindOnly = true;
        initGuardIdx = disposeStack_.back().size();
        disposeStack_.back().push_back(g);
    }
    // fix #747 R43 / §22.11: a class object initializer "assigns through the
    // named properties' setters". An auto-property's setter is a plain field
    // store (its backing field is in `storage` below), but a property with a
    // CUSTOM (block) setter must RUN that setter's body — a direct field store
    // would bypass its logic and any exception it throws. Custom-setter
    // properties are not in `storage` (collectInstanceStorage only records
    // fields + auto-properties), so the assignment was previously dropped
    // silently. Resolve a custom-setter property by name on the class chain.
    auto customSetterPd =
        [&](std::string_view name) -> const ast::PropertyDecl* {
        for (const sema::TypeSymbol* cur = t; cur;) {
            for (const sema::Symbol* mm : cur->members) {
                auto* ps = dynamic_cast<const sema::PropertySymbol*>(mm);
                if (!ps || ps->name != name || ps->isIndexer ||
                    isAutoProperty(ps))
                    continue;
                auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                if (!pd || pd->bodyKind != ast::PropertyBodyKind::Block)
                    return nullptr;
                for (const auto& acc : pd->accessors)
                    if (acc.isSet &&
                        acc.bodyKind != ast::PropertyAccessorBodyKind::Abstract)
                        return pd;
                return nullptr;
            }
            const sema::TypeSymbol* nextBase = nullptr;
            for (const sema::TypeSymbol* b : cur->baseTypes)
                if (b && b->kind == ast::TypeDeclKind::Class) {
                    nextBase = b;
                    break;
                }
            cur = nextBase;
        }
        return nullptr;
    };
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(t, storage);
    for (const auto& a : we.assigns) {
        if (!a.value) continue;
        // §22.11: a custom-setter property target runs its setter body (with
        // `this` = the new instance and `value` = the supplied value) — mirrors
        // lowerPropertySetValue's non-static inlining. If the setter throws, the
        // unwindOnly guard registered above disposes the instance before the
        // exception propagates.
        if (!isRecord) {
            if (const ast::PropertyDecl* pd = customSetterPd(a.name)) {
                const ast::PropertyAccessor* setter = nullptr;
                for (const auto& acc : pd->accessors)
                    if (acc.isSet && acc.bodyKind !=
                            ast::PropertyAccessorBodyKind::Abstract) {
                        setter = &acc;
                        break;
                    }
                llvm::Value* v = lowerExpr(*a.value);
                if (!v || !setter) continue;
                llvm::AllocaInst* vslot = entryAlloca(v->getType());
                builder_.CreateStore(v, vslot);
                locals_[pd] = vslot;  // `value` param anchored on the PropertyDecl
                llvm::Value* savedThis = currentThis_;
                currentThis_ = base;
                llvm::BasicBlock* savedAccEnd = inlinedAccessorEnd_;
                auto* accEndBB =
                    llvm::BasicBlock::Create(llvm_, "with.setter.end", currentFn_);
                inlinedAccessorEnd_ = accEndBB;
                bool term = false;
                if (setter->bodyKind ==
                        ast::PropertyAccessorBodyKind::Expression &&
                    setter->exprBody)
                    lowerExpr(*setter->exprBody);
                else
                    term = lowerScopedBlock(setter->stmts);
                inlinedAccessorEnd_ = savedAccEnd;
                if (!term) builder_.CreateBr(accEndBB);
                builder_.SetInsertPoint(accEndBB);
                currentThis_ = savedThis;
                continue;
            }
        }
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
        // R40 / §9.5: a `with`-replacement of a nullable VALUE-type field must
        // store the tagged {i8,T} struct, not the raw scalar — otherwise the
        // scalar overwrites the hasValue byte and leaves the value half stale
        // (wrong value, or coincidentally non-null). Wrap the replacement value
        // exactly as the positional-constructor path does. (Narrow nullable
        // fields like `byte?`, where the lowered value width differs from the
        // element width, are a separate gap tracked in the R40 follow-up.)
        if (const sema::TypeRef* fty = recordMemberTypeResolved(m)) {
            if (isNullableValueTy(fty) && !v->getType()->isStructTy()) {
                auto* expectedSt = nullableValueStructTy(fty);
                if (v->getType()->isPointerTy() &&
                    llvm::isa<llvm::ConstantPointerNull>(v))
                    v = nullableValueNull(fty);
                else if (v->getType() == expectedSt->getElementType(1))
                    v = wrapNullableValue(fty, v);
            }
        }
        builder_.CreateStore(v, slot);
    }
    // §22.11: the setter sequence completed without throwing — ownership of the
    // instance transfers to the with-expression's result, so remove the implicit
    // try/finally guard (the binding/caller now owns the discharge). Guarded so a
    // value-expression that perturbed the dispose scope cannot erase the wrong
    // entry: only remove it if our exact unwindOnly guard is still at its index.
    if (initGuardIdx != static_cast<std::size_t>(-1) && !disposeStack_.empty() &&
        initGuardIdx < disposeStack_.back().size() &&
        disposeStack_.back()[initGuardIdx].slot == initGuardSlot &&
        disposeStack_.back()[initGuardIdx].unwindOnly) {
        disposeStack_.back().erase(disposeStack_.back().begin() +
                                   static_cast<std::ptrdiff_t>(initGuardIdx));
    }
    // Record: return the modified copy by value. Class: return the mutated
    // instance pointer.
    return isRecord ? builder_.CreateLoad(st, base) : base;
}


// §11.1 / #873: Pre-populate positional-param field slots of BASE classes
// before emitInstanceInit runs the field initializers. Field initializers such
// as `_field := v` resolve `v` to the base class's positional-param FieldSymbol
// (via implicitFieldAddr), so the slot must hold the value passed via the
// `base(...)` chain call BEFORE the initializer evaluates it.
//
// The function is called once per construction site (from lowerNew) with:
//   t           — the TypeSymbol of the class being constructed
//   obj         — the allocated-but-zeroed instance (currentThis_ must be set)
//   ctorArgVals — evaluated constructor arg values [0..n-1], parallel to cs->params
//   cs          — the ConstructorSymbol chosen for this site; null if no explicit ctor
//
// Strategy per class level:
//   (a) If `t` has a TypeDecl with primaryBaseCtorArgs: those expressions are
//       evaluated in the current context (primary-param slots for `t` are already
//       pre-stored by the caller) and stored into the first base's positional slots.
//   (b) If `t` has an explicit ctor (cs != null) with a leading base(...) call:
//       temporarily bind cs->params to ctorArgVals, evaluate the chain-call arg
//       expressions, and store the results into the base's positional-param slots.
//   Recurse into the base class so that multi-level chains (B: A(x), A: Root(x))
//   are handled correctly.
void Emitter::prePopulateBasePositionalFields(
        const sema::TypeSymbol* t,
        llvm::Value* obj,
        const std::vector<llvm::Value*>& ctorArgVals,
        const sema::ConstructorSymbol* cs) {
    if (!t || !obj) return;

    // Find the first base class that has positional (primary-ctor) param fields.
    const sema::TypeSymbol* baseTs = baseClassOf(t);
    if (!baseTs) return;  // no base class, nothing to do

    // Check if the base class has any positional-param fields to populate.
    auto* baseTd = dynamic_cast<const ast::TypeDecl*>(baseTs->decl);
    if (!baseTd || !baseTd->hasPositionalFields || baseTd->positionalFields.empty())
        return;  // base has no primary-ctor params — field inits don't reference them

    // Collect the base's positional FieldSymbols in declaration order.
    std::vector<sema::FieldSymbol*> basePosFields;
    for (sema::Symbol* m : baseTs->members) {
        auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
        if (fs && fs->posParam) basePosFields.push_back(fs);
    }
    if (basePosFields.empty()) return;

    // Determine the arg values to store into the base's positional fields.
    // We look for them from two sources:
    //   (a) TypeDecl::primaryBaseCtorArgs on the CURRENT class (parsed from
    //       the `: Base(args...)` inheritance clause).
    //   (b) The leading base(...) chain call in the explicit ctor body.
    // In both cases, evaluate the arg expressions in the current Emitter context
    // (where primary-param slots for `t` are already populated by the caller).

    std::vector<llvm::Value*> baseArgVals;

    auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);

    // --- Case (a): primary-ctor class with primaryBaseCtorArgs ---
    if (td && td->kind == ast::TypeDeclKind::Class &&
        !td->primaryBaseCtorArgs.empty()) {
        for (const auto& arg : td->primaryBaseCtorArgs) {
            if (!arg.value) { baseArgVals.clear(); break; }
            llvm::Value* v = lowerExpr(*arg.value);
            if (!v) { baseArgVals.clear(); break; }
            baseArgVals.push_back(v);
        }
    }
    // --- Case (b): explicit ctor with leading base(...) chain call ---
    else if (cs && !cs->params.empty() && !ctorArgVals.empty() &&
             cs->params.size() == ctorArgVals.size()) {
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (cd && !cd->body.empty()) {
            bool isThis = false;
            const ast::CallExpr* chainCall = chainCallOf(*cd->body[0], isThis);
            if (chainCall && !isThis && !chainCall->args.empty()) {
                // Temporarily bind ctor params to their values so that the
                // chain-call arg expressions (which reference ctor params) can
                // be evaluated correctly. Save and restore paramSlots_.
                auto savedParams = paramSlots_;
                for (std::size_t pi = 0; pi < cs->params.size(); ++pi) {
                    auto* slot = entryAlloca(ctorArgVals[pi]->getType());
                    builder_.CreateStore(ctorArgVals[pi], slot);
                    paramSlots_[cs->params[pi]] = slot;
                }
                for (const auto& arg : chainCall->args) {
                    if (!arg.value) { baseArgVals.clear(); break; }
                    llvm::Value* v = lowerExpr(*arg.value);
                    if (!v) { baseArgVals.clear(); break; }
                    baseArgVals.push_back(v);
                }
                paramSlots_ = std::move(savedParams);
            }
        }
    }

    if (baseArgVals.empty()) {
        // No base-call args determined — nothing to pre-populate.
        // Still recurse in case the base's own base needs pre-population.
        prePopulateBasePositionalFields(baseTs, obj, {}, nullptr);
        return;
    }

    // Store the base-call arg values into the base's positional-param field slots.
    llvm::StructType* baseSt = classStruct(baseTs);
    std::size_t numToStore = std::min(baseArgVals.size(), basePosFields.size());
    for (std::size_t i = 0; i < numToStore; ++i) {
        sema::FieldSymbol* fs = basePosFields[i];
        auto idxIt = fieldIndex_.find(fs);
        if (idxIt == fieldIndex_.end()) continue;
        llvm::Value* v = baseArgVals[i];
        if (!v) continue;
        llvm::Type* fieldTy = baseSt->getElementType(idxIt->second);
        v = coerceNumericToType(v, fieldTy, /*srcUnsigned=*/false);
        builder_.CreateStore(v, builder_.CreateStructGEP(baseSt, obj, idxIt->second));
    }

    // Recurse: the base class itself may also have a base with positional params
    // (e.g. C(z): B(z): A(z) — B's and A's params both need pre-population).
    // Find what ctor symbol the base class has (if any) for use in recursion.
    const sema::ConstructorSymbol* baseCs = nullptr;
    if (!baseArgVals.empty()) {
        baseCs = ctorByArity(baseTs, baseArgVals.size(), nullptr);
    }
    prePopulateBasePositionalFields(baseTs, obj, baseArgVals, baseCs);
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
                // §4.14: OwnedBuffer<T> uses the same bz_array_t* backing.
                // new OwnedBuffer<T>(capacity) pre-allocates `capacity` slots
                // (all zero-initialized), mirroring the Array no-arg ctor.
                colName == "OwnedBuffer" ||
                colName == "Stack" || colName == "OwnedStack" ||
                colName == "Queue" || colName == "OwnedQueue" ||
                colName == "Set" || colName == "OwnedSet") {
                // For OwnedBuffer<T>(n): the arg is the capacity / initial count.
                // Pre-allocate n slots AND set count=n so index-assignment is valid.
                if (colName == "OwnedBuffer" && !ne.args.empty() && ne.args[0].value) {
                    llvm::Value* nArg = lowerExpr(*ne.args[0].value);
                    if (!nArg) return nullptr;
                    if (!nArg->getType()->isIntegerTy(64))
                        nArg = builder_.CreateSExt(nArg, i64());
                    // bz_array_alloc_count_n: alloc with capacity n and set count=n.
                    // (All slots are zero-initialized by calloc inside the runtime.)
                    auto* fn = declareC("bz_array_alloc_count_n", ptrTy(),
                                       {i64(), i64()});
                    llvm::Value* buf = builder_.CreateCall(fn,
                        {builder_.getInt64(elemSize), nArg}, "ob.arr");
                    // §4.14 (#778): the two-argument constructor
                    // `OwnedBuffer<T>(length, init: (int) -> T)` calls `init(i)`
                    // for each i in 0..<length and stores the result into slot i
                    // (copying for value-typed T, taking ownership for class-typed
                    // T — the buffer's destructor then disposes the owned slots).
                    // Previously the init lambda was ignored and every slot stayed
                    // zero / null.
                    if (ne.args.size() >= 2 && ne.args[1].value) {
                        llvm::Type* elemTy = i32();
                        if (!rt->genericArgs.empty() && rt->genericArgs[0])
                            if (auto* et = lowerTypeRef(rt->genericArgs[0]))
                                elemTy = et;
                        // Lower the init lambda as `(int) -> T` so its param and
                        // return types match (mirrors the LINQ lambda lowering).
                        auto* funcRef = ctx_.makeTypeRef();
                        funcRef->kind = sema::TypeRef::Kind::Function;
                        if (!rt->genericArgs.empty())
                            funcRef->funcReturn = rt->genericArgs[0];
                        sema::TypeRef::FuncParam fp;
                        fp.type = ctx_.primitiveTypeRef(sema::PrimitiveType::Int);
                        funcRef->funcParams.push_back(fp);
                        const sema::TypeRef* savedExpected = expectedFunctionType_;
                        expectedFunctionType_ = funcRef;
                        llvm::Value* initClosure = lowerExpr(*ne.args[1].value);
                        expectedFunctionType_ = savedExpected;
                        if (initClosure) {
                            // for i in 0..<length: slot[i] := init(i)
                            llvm::AllocaInst* iSlot = entryAlloca(i64());
                            builder_.CreateStore(builder_.getInt64(0), iSlot);
                            auto* hdr = llvm::BasicBlock::Create(
                                llvm_, "ob.init.hdr", currentFn_);
                            auto* bodyBB = llvm::BasicBlock::Create(
                                llvm_, "ob.init.body", currentFn_);
                            auto* doneBB = llvm::BasicBlock::Create(
                                llvm_, "ob.init.done", currentFn_);
                            builder_.CreateBr(hdr);
                            builder_.SetInsertPoint(hdr);
                            llvm::Value* iv =
                                builder_.CreateLoad(i64(), iSlot, "ob.i");
                            builder_.CreateCondBr(
                                builder_.CreateICmpSLT(iv, nArg), bodyBB, doneBB);
                            builder_.SetInsertPoint(bodyBB);
                            // call init(i) via the closure ABI {fn_ptr, env_ptr}.
                            llvm::Value* iArg = builder_.CreateTrunc(iv, i32());
                            llvm::Value* clFn =
                                builder_.CreateLoad(ptrTy(), initClosure, "ob.fn");
                            llvm::Value* clEnvSlot = builder_.CreateGEP(
                                ptrTy(), initClosure, {builder_.getInt32(1)});
                            llvm::Value* clEnv =
                                builder_.CreateLoad(ptrTy(), clEnvSlot, "ob.env");
                            auto* clTy = llvm::FunctionType::get(
                                elemTy, {ptrTy(), i32()}, false);
                            llvm::Value* val = builder_.CreateCall(
                                clTy, clFn, {clEnv, iArg}, "ob.v");
                            // store the produced value into slot i.
                            llvm::AllocaInst* vslot = entryAlloca(elemTy);
                            builder_.CreateStore(val, vslot);
                            auto* setFn = declareC(
                                "bz_array_set", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), i64(), ptrTy(), i64()});
                            builder_.CreateCall(setFn,
                                {buf, iv, vslot, builder_.getInt64(elemSize)});
                            builder_.CreateStore(
                                builder_.CreateAdd(iv, builder_.getInt64(1)),
                                iSlot);
                            builder_.CreateBr(hdr);
                            builder_.SetInsertPoint(doneBB);
                        }
                    }
                    return buf;
                }
                // stdlib §4.4/§4.5 (#741): the `constructor(capacity: int)` arg
                // reserves backing slots WITHOUT setting count (an empty array
                // with room to grow), distinct from OwnedBuffer above which also
                // sets count=n. Previously the arg was ignored and a constant 16
                // was always allocated, so `.Capacity` could not reflect the
                // requested reservation. A non-positive request falls back to 16.
                auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* capArg = lowerExpr(*ne.args[0].value);
                    if (capArg) {
                        if (!capArg->getType()->isIntegerTy(64))
                            capArg = builder_.CreateSExt(capArg, i64());
                        // max(capArg, 1) so a 0/negative request still yields a
                        // usable buffer; matches the OwnedArray reference impl
                        // (`if capacity > 0 then capacity else 1`, stdlib §4.5).
                        llvm::Value* one = builder_.getInt64(1);
                        llvm::Value* useArg = builder_.CreateICmpSGT(capArg, one);
                        llvm::Value* cap = builder_.CreateSelect(useArg, capArg, one);
                        return builder_.CreateCall(allocFn,
                            {builder_.getInt64(elemSize), cap}, "col.cap.init");
                    }
                }
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
            // (vtable Dispose + free) for user-defined class T, so the inner T
            // is disposed exactly once when the last strong handle is released
            // (§28.13).
            //
            // §28.13 v0.140 / #689: For compiler-known internally-synchronized
            // types (Mutex, Atomic<T>, AtomicInteger<T>, Semaphore) there is no
            // vtable — they use dedicated C dispose functions. Select the correct
            // disposer based on T's name.
            if (colName == "Shared" && ne.args.size() == 1 && ne.args[0].value) {
                llvm::Value* val = lowerExpr(*ne.args[0].value);
                if (!val) return nullptr;
                // Determine the disposer function for the inner type T.
                llvm::Function* dispFn = nullptr;
                const sema::TypeRef* innerTy =
                    (!rt->genericArgs.empty()) ? rt->genericArgs[0] : nullptr;
                std::string_view tname = innerTy ? builtinTypeName(innerTy) : "";
                if (tname == "Mutex") {
                    // §6.2 / #647 / #689: Mutex has no vtable; uses bz_mutex_dispose.
                    dispFn = declareC("bz_mutex_dispose",
                                      llvm::Type::getVoidTy(llvm_), {ptrTy()});
                } else if (tname == "Atomic" || tname == "AtomicInteger") {
                    // §6.4 / #689 / §28.13 v0.140: Atomic<T>/AtomicInteger<T>
                    // are compiler-known sync types.  Their dispose functions will
                    // be declared when those types land; for now we use null (no
                    // dispose needed for zero-cost atomics — no heap allocation).
                    // TODO: wire up when Atomic<T>/AtomicInteger<T> are implemented.
                    dispFn = nullptr;
                } else if (tname == "Semaphore") {
                    // §6.3 / #689 / §28.13 v0.140: Semaphore has a dedicated
                    // dispose function.  Wire up when Semaphore is implemented.
                    // TODO: wire up when Semaphore is implemented.
                    dispFn = nullptr;
                } else {
                    // User-defined class T or unrecognized builtin: vtable-dispatch
                    // disposer thunk (vtable slot Dispose + free). Same as before.
                    // #602 batch 4 / §28.11 v0.135 / §28.13: ClassOwned row.
                    dispFn = elementDisposerThunk();
                }
                llvm::Value* dispArg = dispFn
                    ? llvm::cast<llvm::Value>(dispFn)
                    : llvm::ConstantPointerNull::get(ptrTy());
                auto* newFn = declareC("bz_shared_new", ptrTy(), {ptrTy(), ptrTy()});
                return builder_.CreateCall(newFn, {val, dispArg}, "shared.new");
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
            // §6.2 / issue #647: new Mutex() — allocate via bz_mutex_new.
            // Mutex is a non-recursive POSIX mutex with refcounted core.
            if (colName == "Mutex") {
                auto* allocFn = declareC("bz_mutex_new", ptrTy(), {});
                return builder_.CreateCall(allocFn, {}, "mx.new");
            }
            // §6.3 / issue #647: new Semaphore(initialCount) or
            //   new Semaphore(initialCount, maxCount) — allocate via
            //   bz_semaphore_new(initial_count, max_count).
            // One-arg form: maxCount defaults to INT_MAX (uncapped).
            // Two-arg form: passes both values.
            if (colName == "Semaphore") {
                // Evaluate initialCount (arg 0).
                llvm::Value* initCount = builder_.getInt32(0);
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* v = lowerExpr(*ne.args[0].value);
                    if (v) {
                        if (!v->getType()->isIntegerTy(32))
                            v = builder_.CreateTrunc(v, i32());
                        initCount = v;
                    }
                }
                // Evaluate maxCount (arg 1); default = INT_MAX if not provided.
                llvm::Value* maxCount = builder_.getInt32(
                    std::numeric_limits<int>::max());
                if (ne.args.size() >= 2 && ne.args[1].value) {
                    llvm::Value* v = lowerExpr(*ne.args[1].value);
                    if (v) {
                        if (!v->getType()->isIntegerTy(32))
                            v = builder_.CreateTrunc(v, i32());
                        maxCount = v;
                    }
                }
                auto* allocFn = declareC("bz_semaphore_new",
                    ptrTy(), {i32(), i32()});
                return builder_.CreateCall(allocFn,
                    {initCount, maxCount}, "sem.new");
            }
            // §6.4 / issue #647: new Atomic<T>(initial) / new AtomicInteger<T>(initial).
            // Allocate via bz_atomic_new(cell_size, initial).
            // cell_size is derived from T: 1 (bool/byte/sbyte/char), 2 (short/ushort),
            //   4 (int/uint — default), 8 (long/ulong/IntPtr/UIntPtr).
            // initial comes from the constructor argument (zero if not provided).
            if (colName == "Atomic" || colName == "AtomicInteger") {
                // Determine cell_size from the type argument T.
                int cellSize = 4;  // default: int/uint
                const sema::TypeRef* newTR = ne.type ? ctx_.resolvedType(*ne.type) : nullptr;
                if (newTR && !newTR->genericArgs.empty() && newTR->genericArgs[0]) {
                    const sema::TypeRef* tArg = newTR->genericArgs[0];
                    if (tArg->kind == sema::TypeRef::Kind::Primitive) {
                        using P = sema::PrimitiveType;
                        switch (tArg->primitive) {
                            case P::Bool: case P::Byte: case P::SByte:
                                cellSize = 1; break;
                            case P::Short: case P::UShort:
                                cellSize = 2; break;
                            case P::Long: case P::ULong:
                                cellSize = 8; break;
                            case P::Char:
                                cellSize = 4; break;  // §6.4: char is 32-bit codepoint
                            default:
                                cellSize = 4; break;  // int/uint/float
                        }
                    } else if (tArg->kind == sema::TypeRef::Kind::Named &&
                               !tArg->declaration && tArg->astNode &&
                               tArg->astNode->name &&
                               !tArg->astNode->name->parts.empty()) {
                        // IntPtr / UIntPtr — pointer-sized = 8 on 64-bit target.
                        cellSize = 8;
                    }
                }
                // Evaluate the initial-value argument (zero if none).
                llvm::Value* initVal = builder_.getInt64(0);
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* raw = lowerExpr(*ne.args[0].value);
                    if (raw) {
                        // Widen to i64 for bz_atomic_new's uint64_t parameter.
                        if (raw->getType()->isIntegerTy(1)) {
                            initVal = builder_.CreateZExt(raw, i64());
                        } else if (raw->getType()->isIntegerTy() &&
                                   !raw->getType()->isIntegerTy(64)) {
                            initVal = builder_.CreateZExt(raw, i64());
                        } else if (raw->getType()->isIntegerTy(64)) {
                            initVal = raw;
                        }
                        // Non-integer arg is ignored (sema would have rejected it).
                    }
                }
                auto* allocFn = declareC("bz_atomic_new", ptrTy(), {i32(), i64()});
                return builder_.CreateCall(
                    allocFn,
                    {builder_.getInt32(cellSize), initVal},
                    "atom.new");
            }
            // issue #545: new Buffer<T>(n) — allocate via bz_buffer_new(len, elem_size).
            // The element size is derived from T (genericArgs[0]) via collectionElemSize-
            // style logic; for decimal (16-byte struct) we use the DataLayout alloc size
            // so the #685 16-byte slot category works correctly.
            // The length argument (n) is required; it is evaluated and passed as i64.
            // Result: opaque heap pointer (bz_buffer_hdr_t*); zero-initialised payload.
            if (colName == "Buffer") {
                // Determine elem_size from T.
                int64_t eSize = 8;  // default (double / long)
                const sema::TypeRef* newTR =
                    ne.type ? ctx_.resolvedType(*ne.type) : nullptr;
                if (newTR && !newTR->genericArgs.empty() && newTR->genericArgs[0]) {
                    const sema::TypeRef* tArg = newTR->genericArgs[0];
                    llvm::Type* et = lowerTypeRef(tArg);
                    if (et) {
                        if (et->isStructTy())
                            eSize = (int64_t)module_->getDataLayout()
                                        .getTypeAllocSize(et);
                        else if (et->isIntegerTy(1) || et->isIntegerTy(8))
                            eSize = 1;
                        else if (et->isIntegerTy(16))
                            eSize = 2;
                        else if (et->isIntegerTy(32) || et->isFloatTy())
                            eSize = 4;
                        else
                            eSize = 8;  // i64 / double / ptr-sized
                    }
                }
                // Evaluate the length argument (required; must be int/long).
                llvm::Value* lenVal = builder_.getInt64(0);
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* raw = lowerExpr(*ne.args[0].value);
                    if (raw) {
                        if (raw->getType()->isIntegerTy(64))
                            lenVal = raw;
                        else if (raw->getType()->isIntegerTy())
                            lenVal = builder_.CreateSExt(raw, i64(), "buf.len64");
                    }
                }
                auto* allocFn = declareC("bz_buffer_new", ptrTy(), {i64(), i64()});
                return builder_.CreateCall(
                    allocFn,
                    {lenVal, builder_.getInt64(eSize)},
                    "buf.new");
            }
            // §6.1 / §30.8: new CancellationTokenSource() — allocate via
            // bz_cts_alloc (stdlib §6.1 default constructor).
            if (colName == "CancellationTokenSource") {
                auto* allocFn = declareC("bz_cts_alloc", ptrTy(), {});
                return builder_.CreateCall(allocFn, {}, "cts.alloc");
            }
            // §8.1 / issue #646: new Stopwatch() — allocate via bz_stopwatch_alloc.
            // Stopwatch is a heap-allocated class with a bz_stopwatch_t layout
            // (start_ts: timespec, elapsed_ns: i64, running: i32).
            if (colName == "Stopwatch") {
                auto* allocFn = declareC("bz_stopwatch_alloc", ptrTy(), {});
                return builder_.CreateCall(allocFn, {}, "sw.alloc");
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

            // §2.15 / issue #687: new JsonOptions(Indented, IndentWidth,
            //   AllowTrailingCommas, IgnoreUnknownFields, MaxDepth)
            // Constructs the bz.json_options value struct from 5 positional args.
            // Fields: {i1 Indented, i32 IndentWidth, i1 AllowTrailingCommas,
            //          i1 IgnoreUnknownFields, i32 MaxDepth}
            if (colName == "JsonOptions") {
                auto* st = jsonOptionsStructType();
                llvm::Value* v = llvm::UndefValue::get(st);
                // Defaults per stdlib §2.15: Indented=false, IndentWidth=2,
                // AllowTrailingCommas=false, IgnoreUnknownFields=false, MaxDepth=64.
                llvm::Value* indented   = builder_.getFalse();
                llvm::Value* indentW    = builder_.getInt32(2);
                llvm::Value* allowTrail = builder_.getFalse();
                llvm::Value* ignoreUnk  = builder_.getFalse();
                llvm::Value* maxDepth   = builder_.getInt32(64);
                if (ne.args.size() >= 1 && ne.args[0].value) {
                    llvm::Value* a = lowerExpr(*ne.args[0].value);
                    if (a) indented = builder_.CreateTrunc(a, i1());
                }
                if (ne.args.size() >= 2 && ne.args[1].value) {
                    llvm::Value* a = lowerExpr(*ne.args[1].value);
                    // Bezel `int` literals lower as i64; truncate to i32 for struct field.
                    if (a) indentW = builder_.CreateTrunc(a, i32(), "jopt.iw32");
                }
                if (ne.args.size() >= 3 && ne.args[2].value) {
                    llvm::Value* a = lowerExpr(*ne.args[2].value);
                    if (a) allowTrail = builder_.CreateTrunc(a, i1());
                }
                if (ne.args.size() >= 4 && ne.args[3].value) {
                    llvm::Value* a = lowerExpr(*ne.args[3].value);
                    if (a) ignoreUnk = builder_.CreateTrunc(a, i1());
                }
                if (ne.args.size() >= 5 && ne.args[4].value) {
                    llvm::Value* a = lowerExpr(*ne.args[4].value);
                    // Bezel `int` literals lower as i64; truncate to i32 for struct field.
                    if (a) maxDepth = builder_.CreateTrunc(a, i32(), "jopt.md32");
                }
                v = builder_.CreateInsertValue(v, indented,   {0u});
                v = builder_.CreateInsertValue(v, indentW,    {1u});
                v = builder_.CreateInsertValue(v, allowTrail, {2u});
                v = builder_.CreateInsertValue(v, ignoreUnk,  {3u});
                v = builder_.CreateInsertValue(v, maxDepth,   {4u});
                return v;
            }

            // §2.15 (issue #550): BinarySerializer / BinaryDeserializer / JsonSerializer / JsonDeserializer.
            // These require `import System.Serialization`; they are compiler-known
            // builtin classes backed by bz_binser_t / bz_binder_t in the runtime.
            // new BinarySerializer() → bz_binser_alloc()  (opaque ptr)
            if (colName == "BinarySerializer" && ne.args.empty()) {
                auto* fn = declareC("bz_binser_alloc", ptrTy(), {});
                return builder_.CreateCall(fn, {}, "binser.new");
            }
            // new JsonSerializer() → bz_jser_alloc() or bz_jser_alloc_opts()
            // fix #687: when options are provided, extract the JsonOptions fields
            // and pass them to bz_jser_alloc_opts().
            if (colName == "JsonSerializer" &&
                (ne.args.empty() ||
                 (ne.args.size() == 1 && ne.args[0].value))) {
                if (ne.args.empty()) {
                    // new JsonSerializer() → bz_jser_alloc()  (defaults)
                    auto* fn = declareC("bz_jser_alloc", ptrTy(), {});
                    return builder_.CreateCall(fn, {}, "jser.new");
                }
                // new JsonSerializer(options: JsonOptions) →
                // bz_jser_alloc_opts(indented, indent_width, max_depth)
                llvm::Value* optsVal = lowerExpr(*ne.args[0].value);
                if (!optsVal) return nullptr;
                llvm::Value* indented  = jsonOptIndented(optsVal);
                llvm::Value* indentW   = jsonOptIndentWidth(optsVal);
                llvm::Value* maxDepth  = jsonOptMaxDepth(optsVal);
                auto* fn = declareC("bz_jser_alloc_opts", ptrTy(),
                                    {i8(), i32(), i32()});
                return builder_.CreateCall(fn, {indented, indentW, maxDepth},
                                           "jser.new");
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
            // new JsonDeserializer(input: ReadOnlySpan<byte>) or
            // new JsonDeserializer(input: string) or
            // new JsonDeserializer(input: ReadOnlySpan<byte>, options: JsonOptions)
            // fix #687: extract JsonOptions from second arg when present.
            if (colName == "JsonDeserializer" && !ne.args.empty() && ne.args[0].value) {
                // Check the first argument type to determine span vs string.
                auto* argTy = ctx_.exprType(*ne.args[0].value);
                bool isString = argTy &&
                    argTy->kind == sema::TypeRef::Kind::Primitive &&
                    argTy->primitive == sema::PrimitiveType::String;
                // Extract options from second arg if present (JsonOptions struct).
                // Defaults per stdlib §2.15.
                llvm::Value* ignoreFld   = builder_.getInt8(0);   // IgnoreUnknownFields
                llvm::Value* allowTrail  = builder_.getInt8(0);   // AllowTrailingCommas
                llvm::Value* maxDepthVal = builder_.getInt32(64); // MaxDepth
                if (ne.args.size() >= 2 && ne.args[1].value) {
                    llvm::Value* optsVal = lowerExpr(*ne.args[1].value);
                    if (optsVal) {
                        ignoreFld   = jsonOptIgnoreUnknown(optsVal);
                        allowTrail  = jsonOptAllowTrailing(optsVal);
                        maxDepthVal = jsonOptMaxDepth(optsVal);
                    }
                }
                if (isString) {
                    // new JsonDeserializer(string) — string is a bz_string*.
                    llvm::Value* strPtr = lowerExpr(*ne.args[0].value);
                    if (!strPtr) return nullptr;
                    auto* utf8zFn = declareC("bz_string_to_utf8z", ptrTy(), {ptrTy()});
                    llvm::Value* utf8z = builder_.CreateCall(utf8zFn, {strPtr}, "jder.utf8z");
                    auto* strlenFn = declareC("strlen", i64(), {ptrTy()});
                    llvm::Value* utf8Len = builder_.CreateCall(strlenFn, {utf8z}, "jder.utf8len");
                    // fix #687: use bz_jder_alloc_str_opts (ignore_unknown,
                    // allow_trailing, max_depth).
                    auto* fn = declareC("bz_jder_alloc_str_opts", ptrTy(),
                                        {ptrTy(), i64(), i8(), i8(), i32()});
                    return builder_.CreateCall(fn,
                        {utf8z, utf8Len, ignoreFld, allowTrail, maxDepthVal},
                        "jder.new");
                } else {
                    // new JsonDeserializer(ReadOnlySpan<byte>[, options]) — unpack span.
                    llvm::Value* spanVal = lowerExpr(*ne.args[0].value);
                    if (!spanVal) return nullptr;
                    llvm::Value* dataPtr = spanSource(spanVal);
                    llvm::Value* lenI32  = spanLength(spanVal);
                    llvm::Value* arrData = builder_.CreateLoad(ptrTy(), dataPtr, "jder.data");
                    llvm::Value* startI32 = spanStart(spanVal);
                    llvm::Value* startI64 = builder_.CreateZExt(startI32, i64(), "jder.s64");
                    llvm::Value* lenI64   = builder_.CreateZExt(lenI32, i64(), "jder.l64");
                    llvm::Value* byteData = builder_.CreateGEP(
                        llvm::Type::getInt8Ty(llvm_), arrData, startI64, "jder.bdata");
                    // fix #687: use bz_jder_alloc_opts (ignore_unknown, allow_trailing, max_depth).
                    auto* fn = declareC("bz_jder_alloc_opts", ptrTy(),
                                        {ptrTy(), i64(), i8(), i8(), i32()});
                    return builder_.CreateCall(fn,
                        {byteData, lenI64, ignoreFld, allowTrail, maxDepthVal},
                        "jder.new");
                }
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
            // #732: if EVERY generic arg is an UNBOUND type parameter (a generic
            // METHOD template body, e.g. `new Box<T>` inside `Make<T>` where T
            // is not bound in the active subst), do NOT install a substitution.
            // Installing one would lowerTypeRef(T) → the i32 fallback and
            // specialize the record (`record.Box$spec0`), but the function's
            // declared return type `Box<T>` was emitted UNspecialized
            // (`record.Box`, via recordStruct's matching allUnbound path), so the
            // produced value would mismatch the return type → IR-verify failure.
            // Mirrors the allUnbound test in LlvmBackend_Layout.cpp:594-609.
            bool allUnbound = true;
            for (const auto& arg : rt->genericArgs) {
                // issue #787: a bare type parameter (`T`) has NO generic args of
                // its own; a builtin collection like `Array<int>` is ALSO a
                // Named/declaration-less/single-part name ("Array") not in the
                // subst, so it was misclassified as unbound — `new Box<Array<int>>`
                // then SKIPPED the substitution, built the UNSPECIALIZED {i32}
                // struct, and truncated the 8-byte Array pointer into a 4-byte
                // field (SIGSEGV on field access via the specialized {ptr} view).
                // A type carrying its own generic args is concrete, not unbound.
                if (!arg || arg->kind != sema::TypeRef::Kind::Named ||
                    arg->declaration || !arg->astNode || !arg->astNode->name ||
                    arg->astNode->name->parts.size() != 1 ||
                    !arg->genericArgs.empty() ||
                    genericSubst_.count(arg->astNode->name->parts[0])) {
                    allUnbound = false;
                    break;
                }
            }
            // Resolve generic args WITH the outer substitution active so
            // that `new Pair<T>` inside a `Pair<string>` method correctly
            // maps T → ptr (string) rather than the i32 fallback.
            if (!allUnbound) {
                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
                for (std::size_t gi = 0; gi < td->genericParams.size(); ++gi) {
                    llvm::Type* concrTy = lowerTypeRef(rt->genericArgs[gi]);
                    std::string_view pname = td->genericParams[gi].name;
                    newSubst[pname] = concrTy;
                    // Record the sema TypeRef for the concrete arg so the
                    // binary `=` handler can distinguish string vs. class ptr.
                    // §14.1 (#766): resolve a bare-param arg through the outer subst.
                    newSema[pname] = resolveTypeParamRef(rt->genericArgs[gi]);
                }
                genericSubst_ = std::move(newSubst);
                genericSubstSemaTy_ = std::move(newSema);
            }
        }
    } else if (!rt->genericArgs.empty() && !t->decl &&
               t->genericParams.size() == rt->genericArgs.size()) {
        // #428 / §14.1 / §2.5: IMPORTED generic type — no ast::TypeDecl is
        // available (decl is null for imported TypeSymbols), but the generic
        // parameter names are in t->genericParams (GenericParamSymbol* with
        // .name, populated by ModuleLoader::convertGenericParams).  Build the
        // same per-instantiation substitution so recordStruct (and classStruct)
        // compute correct field layouts for non-int type arguments (the
        // prior silent miscompile was due to this substitution being skipped
        // entirely for imported types, leaving T's i32 placeholder in place).
        std::unordered_map<std::string_view, llvm::Type*> newSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
        for (std::size_t gi = 0; gi < t->genericParams.size(); ++gi) {
            llvm::Type* concrTy = lowerTypeRef(rt->genericArgs[gi]);
            std::string_view pname = t->genericParams[gi]->name;
            newSubst[pname] = concrTy;
            // §14.1 (#766): resolve a bare-param arg through the outer subst.
            newSema[pname] = resolveTypeParamRef(rt->genericArgs[gi]);
        }
        genericSubst_ = std::move(newSubst);
        genericSubstSemaTy_ = std::move(newSema);
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

    // §14.1 / §2.5 / §2.6 (issue #328 FINDING [2]; #428 / #662 foundation A):
    // an IMPORTED GENERIC type has NO stable per-type symbols to link against —
    // a generic is monomorphized per-instantiation (§14), so the dependency
    // emits no `bz$<Type>$init` (only non-generic publics export an init) and
    // its `bz$<Type>$ctor` is the template (T→i32), not this instantiation.
    //
    // Foundation A (#662): we now support construction of imported generic
    // CLASS types that have NO explicit constructor (implicit default = zero
    // args, zero field initializers).  For those:
    //   - the per-instantiation vtable is built by buildImportedGenericVtable
    //     (called from emitInstanceInit below) with genericSubst_ active.
    //   - there is no ctor body to inline and no external ctor ABI to match.
    //   - the importer emits: calloc → install vtable → (no ctor call) → done.
    //   - sema narrowed the §14 gate: !hasAnyConstructor → accepted.
    //
    // Classes WITH an explicit constructor remain gated: the ctor body is
    // absent from the metadata (Foundation C / Blocker 3).  Records ride the
    // #428b path.  The backstop comment applies to those still-gated shapes.
    // §11.1 / #873: when lowerNew pre-evaluates the ctor args (to allow
    // prePopulateBasePositionalFields to bind params before emitInstanceInit),
    // the results are stored here and reused by the ctor-call section below so
    // that arg expressions with side effects are never evaluated twice.
    // Only populated for local (non-imported, non-generic) classes with an
    // explicit ctor matching the call's arity.
    std::vector<llvm::Value*>      prebuiltCtorArgs;    // args[0]=obj, [1..n]=params
    std::vector<const ast::Expr*>  prebuiltCtorExprs;   // parallel to [1..n]
    sema::ConstructorSymbol*       prebuiltCtorCs   = nullptr;
    llvm::Function*                prebuiltCtorFn   = nullptr;

    const bool importedGeneric =
        ctx_.isImported(t) && !t->genericParams.empty();
    // #662 foundation A: an imported generic class with no explicit ctor uses
    // the local emitInstanceInit path so the per-instantiation vtable is
    // installed. The genericSubst_ is already set above (imported-type branch).
    const bool importedGenericNoBody =
        importedGeneric && t->kind == ast::TypeDeclKind::Class &&
        !hasAnyConstructor(t);
    // #662 foundation C: imported generic class WITH a loaded ctor body.
    // The ctor body was re-parsed + sema'd in the loader; cs->decl is set.
    // We install the per-instantiation vtable via emitInstanceInit (same as
    // Foundation A), then fall through to the inline-ctor path below.
    const sema::ConstructorSymbol* importedGenericExplicitCs = nullptr;
    if (importedGeneric && !importedGenericNoBody &&
        t->kind == ast::TypeDeclKind::Class) {
        for (sema::Symbol* m : t->members) {
            auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
            if (cs && !cs->isStatic && cs->decl != nullptr) {
                importedGenericExplicitCs = cs;
                break;
            }
        }
    }
    const bool importedGenericWithBody =
        importedGenericExplicitCs != nullptr;
    if (ctx_.isImported(t) && !importedGeneric) {
        llvm::Function* initFn = declareImportedInit(t);
        builder_.CreateCall(initFn, {obj});
    } else if (importedGenericNoBody || importedGenericWithBody) {
        // Foundation A: no explicit ctor — install vtable, no ctor body.
        // Foundation C: explicit ctor with loaded body — install vtable first,
        // then fall through to the inline-ctor path (hasAnyConstructor guard).
        emitInstanceInit(t, obj, st);
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
        // §11.1 / #873: pre-populate base classes' positional-param field slots
        // BEFORE emitInstanceInit so that base field initializers (e.g.
        // `_field := v`) can read the value passed via the base(v) chain call
        // rather than the calloc-zeroed default. Evaluate the ctor args once
        // here (under the outer substitution, matching the ctor-call path below)
        // so they can be reused by the ctor call WITHOUT re-evaluation.
        //
        // Two cases for currentThis_ during arg evaluation:
        //   (a) primaryBaseCtorArgs (class-header `Base(v)` form, no explicit ctor):
        //       `v` is a primary-ctor FieldSymbol on `t` → implicitFieldAddr GEPs
        //       against `obj` → keep currentThis_ = obj.
        //   (b) explicit ctor call-site args (`new Pair<T>(_second, _first)`):
        //       `_second`/`_first` are fields of the ENCLOSING scope's `this`,
        //       not the new object → must evaluate under savedThisForInit.
        // Explicit ctor path (b) saves/restores currentThis_ around buildOrderedArgs.
        {
            auto ctorArgSubstSave = genericSubst_;
            auto ctorArgSemaSave  = genericSubstSemaTy_;
            genericSubst_       = savedSubst;
            genericSubstSemaTy_ = savedSemaTy;
            if (sema::ConstructorSymbol* earlyCs = ctorForArgs(t, ne.args)) {
                auto earlyIt = ctorFns_.find(earlyCs);
                auto* earlyCd =
                    dynamic_cast<const ast::ConstructorDecl*>(earlyCs->decl);
                if (earlyIt != ctorFns_.end() && earlyCd) {
                    std::vector<llvm::Value*> eArgs;
                    eArgs.push_back(obj);
                    // Collect arg exprs for +1-string-release bookkeeping later.
                    std::vector<const ast::Expr*> eArgExprs;
                    // (b): explicit ctor args belong to the enclosing scope —
                    // evaluate under savedThisForInit so implicit field accesses
                    // (e.g. `_second` in `new Pair<T>(_second, _first)`) resolve
                    // against the enclosing `this`, not the new object.
                    llvm::Value* savedThisForArgs = currentThis_;
                    currentThis_ = savedThisForInit;
                    bool ok = buildOrderedArgs(earlyCd->params, ne.args, eArgs,
                                              &eArgExprs);
                    currentThis_ = savedThisForArgs;
                    if (ok) {
                        // Store for the ctor-call section below to reuse.
                        prebuiltCtorArgs  = std::move(eArgs);
                        prebuiltCtorExprs = std::move(eArgExprs);
                        prebuiltCtorCs    = earlyCs;
                        prebuiltCtorFn    = earlyIt->second;
                        // Pre-populate base positional-param fields using
                        // args[1..n] (param values, not the `this` in args[0]).
                        std::vector<llvm::Value*> paramVals(
                            prebuiltCtorArgs.begin() + 1,
                            prebuiltCtorArgs.end());
                        prePopulateBasePositionalFields(t, obj, paramVals, earlyCs);
                    }
                }
            }
            if (prebuiltCtorArgs.empty()) {
                // No explicit ctor (primary-ctor class with primaryBaseCtorArgs
                // or no chain at all): still pre-populate from primaryBaseCtorArgs.
                // (a): primaryBaseCtorArgs expressions reference primary-ctor
                // FieldSymbols on `t` → evaluated via implicitFieldAddr(GEP obj),
                // so currentThis_ = obj is already correct here.
                prePopulateBasePositionalFields(t, obj, {}, nullptr);
            }
            genericSubst_       = std::move(ctorArgSubstSave);
            genericSubstSemaTy_ = std::move(ctorArgSemaSave);
        }
        emitInstanceInit(t, obj, st);
        currentThis_ = savedThisForInit;
    }
    if (importedGeneric && !importedGenericNoBody && !importedGenericWithBody) {
        // Still-gated shape (explicit ctor but body not loaded): return as-is.
        // The backstop prevents codegen from emitting a dangling external
        // bz$<Type>$init / bz$<Type>$ctor reference (the FINDING [2] link
        // failure).  Foundation A+C shapes fall through below so that
        // the hasAnyConstructor guard chain (inline-ctor call) runs.
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
    // §14/#662 Foundation C: imported generic CLASS with a loaded ctor body.
    // Inline the ctor body (same inline path as local generics) with
    // genericSubst_ active so field GEPs use the spec struct layout and
    // parameter stores use the concrete types.  The vtable was already installed
    // by emitInstanceInit above (importedGenericWithBody branch).
    if (importedGenericWithBody) {
        auto* cs = const_cast<sema::ConstructorSymbol*>(importedGenericExplicitCs);
        // Build arg list: lower under the OUTER subst (savedSubst) — the args are
        // caller-scope expressions — then restore the new type's subst for
        // the inline body, matching the local-generic path (#597).
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (cd) {
            std::vector<llvm::Value*> args;
            args.push_back(obj);
            auto ctorArgSubst = genericSubst_;
            auto ctorArgSemaTy = genericSubstSemaTy_;
            genericSubst_ = savedSubst;
            genericSubstSemaTy_ = savedSemaTy;
            std::vector<const ast::Expr*> ctorArgExprs;
            bool builtArgs = buildOrderedArgs(cd->params, ne.args, args, &ctorArgExprs);
            genericSubst_ = std::move(ctorArgSubst);
            genericSubstSemaTy_ = std::move(ctorArgSemaTy);
            if (builtArgs && args.size() == cs->params.size() + 1) {
                llvm::Value* savedThis = currentThis_;
                auto savedLocals = locals_;
                auto savedParams = paramSlots_;
                llvm::BasicBlock* savedUnwindTarget = unwindTarget_;
                std::size_t savedUnwindDepth = unwindScopeDepth_;
                currentThis_ = obj;
                locals_.clear();
                paramSlots_.clear();
                // Bind ctor params to the lowered arg values.
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
                // Release any +1 string producer ctor args after body completes.
                if (!diverged_) {
                    for (std::size_t ai = 1;
                         ai < args.size() && ai < ctorArgExprs.size() + 1; ++ai) {
                        if (ctorArgExprs[ai - 1] &&
                            isPlusOneStringProducer(ctorArgExprs[ai - 1]))
                            emitStringRelease(args[ai]);
                    }
                }
            }
        }
        emitNewPendingCheck(obj);
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        return obj;
    }

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
            std::vector<const ast::Expr*> ctorArgExprs;
            bool builtArgs = false;
            // §11.1 / #873: reuse pre-evaluated ctor args from the
            // prePopulateBasePositionalFields step above (to avoid
            // double-evaluating side-effecting arg expressions).
            if (cs == prebuiltCtorCs && it->second == prebuiltCtorFn &&
                !prebuiltCtorArgs.empty()) {
                args        = std::move(prebuiltCtorArgs);
                ctorArgExprs = std::move(prebuiltCtorExprs);
                builtArgs   = true;
            } else {
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
            args.push_back(obj);
            // #429 (ARGUMENT-PASS category): collect per-param arg expressions
            // so we can release any +1 string producer after the ctor runs.
            builtArgs = buildOrderedArgs(cd->params, ne.args, args,
                                              &ctorArgExprs);
            genericSubst_ = std::move(ctorArgSubst);
            genericSubstSemaTy_ = std::move(ctorArgSemaTy);
            }
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
                // #429 (ARGUMENT-PASS category) / §4.6 / §28.9: release any
                // +1 string producer constructor arguments after the ctor body
                // runs. Constructor params are borrows; field-store of a param
                // retains independently (rc→2) so our release (rc→1) is safe.
                // args[0]=obj; args[1..n]=ctor params; ctorArgExprs is parallel.
                if (!diverged_) {
                    for (std::size_t ai = 1;
                         ai < args.size() && ai < ctorArgExprs.size() + 1; ++ai) {
                        if (ctorArgExprs[ai - 1] &&
                            isPlusOneStringProducer(ctorArgExprs[ai - 1]))
                            emitStringRelease(args[ai]);
                    }
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
    // int → decimal (§5.1, #746 R41-I): an IMPLICIT integer-to-decimal widening
    // (`var m: decimal := i;`) must use the same runtime conversion the explicit
    // `decimal(i)` cast does (emitIntToDecimal), not a raw store of the integer
    // bits into the 16-byte decimal struct (which produced a misrepresented
    // value that failed equality with a decimal literal).
    if (srcTy->isIntegerTy() && !srcTy->isIntegerTy(1) &&
        targetTy == decimalStructType()) {
        using P = sema::PrimitiveType;
        llvm::Value* iv = v;
        P sp;
        unsigned bw = srcTy->getIntegerBitWidth();
        if (bw <= 32) {
            if (bw < 32)
                iv = srcUnsigned ? builder_.CreateZExt(v, i32())
                                 : builder_.CreateSExt(v, i32());
            sp = srcUnsigned ? P::UInt : P::Int;
        } else {
            sp = srcUnsigned ? P::ULong : P::Long;
        }
        llvm::AllocaInst* out = entryAlloca(decimalStructType());
        emitIntToDecimal(iv, sp, out);
        return builder_.CreateLoad(decimalStructType(), out);
    }
    return v;  // no §5.1 numeric widening applies — leave the value as is.
}


// §5.3 (#839): scalar-level primitive conversion shared by lowerConversion
// (non-nullable target) and the NullCondCallExpr nullable-cast path. Takes
// the ALREADY-UNWRAPPED `payload` value (of source primitive `srcPt`) and
// converts it to `targetPt` using the §5.2 rules:
//   • same type → identity (with signed↔unsigned bit-width check per §5.2/#753)
//   • FP ↔ FP   → FPExt / FPTrunc
//   • FP → int  → range-checked FPToSI/FPToUI (OverflowException)
//   • int → FP  → SIToFP / UIToFP (lossless for all supported widths)
//   • int → int → widening SExt/ZExt or narrowing Trunc with range check
//   • int → char → §5.2 L421 Unicode range check + Trunc
// `srcUns` is isUnsignedPrim(srcPt), pre-computed by the caller.
llvm::Value* Emitter::lowerPrimCastScalar(sema::PrimitiveType targetPt,
                                          sema::PrimitiveType srcPt,
                                          llvm::Value* payload,
                                          bool srcUns) {
    using P = sema::PrimitiveType;
    // §5.2 (L421): casting an integer to char requires the value to be in
    // U+0000–U+10FFFF excluding the surrogate range U+D800–U+DFFF.
    if (targetPt == P::Char && payload->getType()->isIntegerTy()) {
        llvm::Type* i64ty = llvm::Type::getInt64Ty(llvm_);
        llvm::Value* v64  = builder_.CreateSExt(payload, i64ty, "ctoi64");
        llvm::Value* neg  = builder_.CreateICmpSLT(
            v64, llvm::ConstantInt::get(i64ty, 0), "cneg");
        llvm::Value* hi   = builder_.CreateICmpSGT(
            v64, llvm::ConstantInt::get(i64ty, 0x10FFFF), "chi");
        llvm::Value* sg_lo = builder_.CreateICmpSGE(
            v64, llvm::ConstantInt::get(i64ty, 0xD800), "csglo");
        llvm::Value* sg_hi = builder_.CreateICmpSLE(
            v64, llvm::ConstantInt::get(i64ty, 0xDFFF), "csghi");
        llvm::Value* surr  = builder_.CreateAnd(sg_lo, sg_hi, "csurr");
        llvm::Value* bad   = builder_.CreateOr(
            builder_.CreateOr(neg, hi, "coor"), surr, "cbad");
        emitGuardThrow(bad, "OverflowException");
        llvm::Type* dstCharTy = lowerPrimitive(P::Char);  // i32
        if (payload->getType() != dstCharTy)
            return builder_.CreateTrunc(payload, dstCharTy);
        return payload;
    }
    llvm::Type* dstTy = lowerPrimitive(targetPt);
    llvm::Type* srcTy = payload->getType();
    bool dstFP  = dstTy->isFloatingPointTy();
    bool srcFP  = srcTy->isFloatingPointTy();
    bool dstUns = isUnsignedPrim(targetPt);
    if (srcTy == dstTy) {
        // §5.2 (#753): same-width signed↔unsigned cast is a CHECKED conversion.
        if (srcTy->isIntegerTy() && srcUns != dstUns) {
            llvm::Value* bad = builder_.CreateICmpSLT(
                payload, llvm::ConstantInt::get(srcTy, 0), "swflip.oor");
            emitGuardThrow(bad, "OverflowException");
        }
        return payload;
    }
    if (srcFP && dstFP) {
        return srcTy->getPrimitiveSizeInBits() < dstTy->getPrimitiveSizeInBits()
               ? builder_.CreateFPExt(payload, dstTy)
               : builder_.CreateFPTrunc(payload, dstTy);
    }
    if (srcFP) {  // FP → int: range-checked per §5.2.
        auto pow2 = [](unsigned e) {
            double v = 1.0;
            for (unsigned i = 0; i < e; ++i) v *= 2.0;
            return v;
        };
        unsigned nbits = dstTy->getIntegerBitWidth();
        double maxBound = dstUns ? pow2(nbits) : pow2(nbits - 1);
        llvm::Value* maxC  = llvm::ConstantFP::get(srcTy, maxBound);
        llvm::Value* ltMax = builder_.CreateFCmpOLT(payload, maxC, "f2i.ltmax");
        unsigned srcMantBits = srcTy->isFloatTy() ? 24u : 53u;
        bool minMinus1Exact = dstUns || (nbits - 1) < srcMantBits;
        llvm::Value* geMin;
        if (minMinus1Exact) {
            double minExcl = (dstUns ? 0.0 : -pow2(nbits - 1)) - 1.0;
            geMin = builder_.CreateFCmpOGT(
                payload, llvm::ConstantFP::get(srcTy, minExcl), "f2i.gtmin");
        } else {
            geMin = builder_.CreateFCmpOGE(
                payload, llvm::ConstantFP::get(srcTy, -pow2(nbits - 1)),
                "f2i.gemin");
        }
        llvm::Value* inRange = builder_.CreateAnd(geMin, ltMax, "f2i.inrange");
        emitGuardThrow(builder_.CreateNot(inRange, "f2i.oor"), "OverflowException");
        return dstUns ? builder_.CreateFPToUI(payload, dstTy)
                      : builder_.CreateFPToSI(payload, dstTy);
    }
    if (dstFP)  // int → FP
        return srcUns ? builder_.CreateUIToFP(payload, dstTy)
                      : builder_.CreateSIToFP(payload, dstTy);
    // int → int.
    unsigned sb = srcTy->getIntegerBitWidth();
    unsigned db = dstTy->getIntegerBitWidth();
    if (db < sb) {
        // Narrowing: checked per §5.2.
        llvm::Value* trunc = builder_.CreateTrunc(payload, dstTy);
        llvm::Value* back = dstUns ? builder_.CreateZExt(trunc, srcTy)
                                   : builder_.CreateSExt(trunc, srcTy);
        emitGuardThrow(builder_.CreateICmpNE(back, payload), "OverflowException");
        return trunc;
    }
    if (db > sb) {
        if (srcTy->isIntegerTy(1))
            return builder_.CreateZExt(payload, dstTy);  // bool→int always zero-ext
        return srcUns ? builder_.CreateZExt(payload, dstTy)
                      : builder_.CreateSExt(payload, dstTy);
    }
    return payload;  // same-width, same-sign — already handled above; unreachable
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

    // §5.3 (#827): an explicit cast of a NULLABLE value to a NON-NULLABLE
    // primitive target unwraps the nullable first — "casting a nullable to its
    // non-nullable form requires the value to be non-null at runtime", raising
    // NullReferenceException when null (mirrors the postfix `!` unwrap). Without
    // this the {i8, T} nullable struct flowed into the scalar conversion paths
    // below as if it were a bare T: `int(n:int?)` aborted on a cast<IntegerType>
    // and `double(n:double?)` emitted `sitofp` on the struct (IR_FAIL). A
    // nullable TARGET (`long?(x)`, §5.3 "null stays null") keeps the struct and
    // is handled by its own lifted path, so guard on the result being
    // non-nullable.
    {
        bool targetNullable = false;
        if (auto* ct = ctx_.exprType(call)) targetNullable = ct->nullable;
        if (!targetNullable && src->getType()->isStructTy()) {
            bool isNullableVal = false;
            if (auto* rt = ctx_.exprType(argE))
                isNullableVal = isNullableValueTy(rt);
            auto* st = llvm::cast<llvm::StructType>(src->getType());
            if (!isNullableVal && st->getNumElements() >= 1 &&
                st->getElementType(0) == nullablePrimHasValTy())
                isNullableVal = true;  // robust LLVM-shape fallback
            if (isNullableVal) {
                emitGuardThrow(builder_.CreateNot(nullablePrimHasValue(src)),
                               "NullReferenceException");
                src = nullablePrimValue(src);
            }
        }
    }

    // §5.2 / fix #664: decimal is a 16-byte struct, not a scalar — the
    // generic FP/integer cast paths below would crash on it.  Handle the four
    // decimal conversion directions here, before any scalar-type logic runs.
    //
    //   int/double → decimal : bz_decimal_from_int32/int64/double
    //   decimal → int        : bz_decimal_to_int32/int64 (truncate toward zero)
    //   decimal → double     : bz_decimal_to_double
    {
        // Determine source primitive from the sema type annotation.
        P srcPrim = P::None;
        if (auto* rt = ctx_.exprType(argE))
            if (rt->kind == sema::TypeRef::Kind::Primitive)
                srcPrim = rt->primitive;

        llvm::StructType* decTy = decimalStructType();

        // Helper: get OverflowException vtable pointer (may be null if not set up yet).
        auto getOVEVtable = [&]() -> llvm::Value* {
            auto vit = builtinVtable_.find("OverflowException");
            if (vit == builtinVtable_.end())
                return llvm::ConstantPointerNull::get(ptrTy());
            return vit->second;
        };

        // Case A: integer or double → decimal (target == P::Decimal).
        if (target == P::Decimal) {
            llvm::AllocaInst* outSlot = entryAlloca(decTy);
            if (src->getType()->isFloatingPointTy()) {
                // double/float → decimal: bz_decimal_from_double (§5.2: explicit).
                // Widen float to double first (the runtime takes double).
                llvm::Value* dv = src;
                if (!src->getType()->isDoubleTy())
                    dv = builder_.CreateFPExt(src, llvm::Type::getDoubleTy(llvm_));
                auto* fn = declareC("bz_decimal_from_double",
                                   llvm::Type::getVoidTy(llvm_),
                                   {llvm::Type::getDoubleTy(llvm_), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {dv, outSlot, getOVEVtable()});
                emitPendingCheck();  // OverflowException from out-of-range double
            } else if (src->getType()->isIntegerTy()) {
                // int/long (and narrower integers) → decimal: widen to int32/int64
                // then call the appropriate from_int.  This covers the explicit
                // decimal(x) cast (§5.2) the same way the implicit §5.1 widening
                // in lowerBinary does it.
                // #743: honour unsigned sources (ulong/uint above the signed
                // max) — emitIntToDecimal picks the correct conversion.
                emitIntToDecimal(src, srcPrim, outSlot);
            } else {
                return nullptr;  // unrecognised source — should not occur after sema
            }
            return builder_.CreateLoad(decTy, outSlot, "dec.conv");
        }

        // Case B: decimal → int (any integer target). §5.2: truncate toward zero;
        // OverflowException when the truncated value is out of the target's range.
        if (srcPrim == P::Decimal && src->getType() == decTy) {
            if (lowerPrimitive(target)->isIntegerTy()) {
                llvm::AllocaInst* srcSlot = entryAlloca(decTy);
                builder_.CreateStore(src, srcSlot);
                if (lowerPrimitive(target)->getIntegerBitWidth() <= 32) {
                    // Target fits in int32 (byte, sbyte, short, ushort, int, char).
                    // §5.2, fix #872: pass the OverflowException vtable so that a
                    // decimal exceeding int64 range (e.g. 1e19) raises OverflowException
                    // here; a decimal in int64 range but out of int32 range is caught by
                    // the IR range-check below.  The vtable path is the same one used by
                    // the int64-target branch and works in the JIT harness in practice.
                    auto* fn64 = declareC("bz_decimal_to_int64", i64(),
                                         {ptrTy(), ptrTy()});
                    llvm::Value* v64 = builder_.CreateCall(
                        fn64, {srcSlot, getOVEVtable()},
                        "dec.toi64v");
                    emitPendingCheck();  // §5.2, #872: raises if decimal > int64 range
                    // Range check: [-2147483648 .. 2147483647]
                    llvm::Value* lo = builder_.CreateICmpSLT(
                        v64, llvm::ConstantInt::get(i64(), (int64_t)INT32_MIN),
                        "dec.ilo");
                    llvm::Value* hi = builder_.CreateICmpSGT(
                        v64, llvm::ConstantInt::get(i64(), (int64_t)INT32_MAX),
                        "dec.ihi");
                    emitGuardThrow(builder_.CreateOr(lo, hi), "OverflowException");
                    llvm::Value* v32 = builder_.CreateTrunc(v64, i32(), "dec.toi32");
                    llvm::Type* dstTy = lowerPrimitive(target);
                    if (dstTy == i32()) return v32;
                    // Further narrowing (e.g. to i8/i16): check sub-range and trunc.
                    bool dstUns = isUnsignedPrim(target);
                    llvm::Value* trunc = builder_.CreateTrunc(v32, dstTy);
                    llvm::Value* back = dstUns
                        ? builder_.CreateZExt(trunc, i32())
                        : builder_.CreateSExt(trunc, i32());
                    emitGuardThrow(builder_.CreateICmpNE(back, v32), "OverflowException");
                    return trunc;
                } else {
                    // int64 or uint64 target.
                    auto* fn = declareC("bz_decimal_to_int64", i64(),
                                       {ptrTy(), ptrTy()});
                    llvm::Value* v64 = builder_.CreateCall(fn, {srcSlot, getOVEVtable()},
                                                           "dec.toi64");
                    emitPendingCheck();
                    return v64;
                }
            }
            // Case C: decimal → double/float. §5.2: "double(m)" — bz_decimal_to_double.
            if (lowerPrimitive(target)->isFloatingPointTy()) {
                llvm::AllocaInst* srcSlot = entryAlloca(decTy);
                builder_.CreateStore(src, srcSlot);
                auto* fn = declareC("bz_decimal_to_double",
                                   llvm::Type::getDoubleTy(llvm_),
                                   {ptrTy()});
                llvm::Value* dv = builder_.CreateCall(fn, {srcSlot}, "dec.todbl");
                llvm::Type* dstTy = lowerPrimitive(target);
                if (dstTy->isDoubleTy()) return dv;
                // Narrow to float if target is float.
                return builder_.CreateFPTrunc(dv, dstTy);
            }
        }
    }

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
    bool dstFP = dstTy->isFloatingPointTy();
    bool srcFP = srcTy->isFloatingPointTy();
    bool dstUns = isUnsignedPrim(target);
    bool srcUns = isUnsignedTargetExpr(argE);
    // §10.3 / §5.4 (#740): when the source is an enum, its signedness is that of
    // its underlying type — so `int(byteEnum)` zero-extends an unsigned byte
    // value rather than sign-corrupting it.
    if (auto* st = ctx_.exprType(argE))
        if (st->kind == sema::TypeRef::Kind::Named && st->declaration &&
            st->declaration->kind == ast::TypeDeclKind::Enum)
            srcUns = enumIsUnsigned(st->declaration);
    if (srcTy == dstTy) {
        // §5.2 (#753): a same-width signed↔unsigned integer cast is a CHECKED
        // value conversion — it raises OverflowException when the value is not
        // representable under the destination's signedness. At equal LLVM
        // integer width the only out-of-range values are those with the top bit
        // set: read as signed they are negative (cannot fit an unsigned
        // destination); read as unsigned they exceed the signed maximum (cannot
        // fit a signed destination). Either way ICmpSLT(src, 0) on the shared
        // bit pattern detects them. Bit reinterpretation WITHOUT this check is
        // BitConverter.Reinterpret<T> (stdlib §2.12).
        if (srcTy->isIntegerTy() && srcUns != dstUns) {
            llvm::Value* bad = builder_.CreateICmpSLT(
                src, llvm::ConstantInt::get(srcTy, 0), "swflip.oor");
            emitGuardThrow(bad, "OverflowException");
        }
        return src;
    }
    if (srcFP && dstFP) {
        return srcTy->getPrimitiveSizeInBits() <
                       dstTy->getPrimitiveSizeInBits()
                   ? builder_.CreateFPExt(src, dstTy)
                   : builder_.CreateFPTrunc(src, dstTy);
    }
    if (srcFP) {  // float → int
        // §5.2: "Floating-point to integer truncates toward zero; NaN,
        // ±Infinity, and out-of-range values raise OverflowException." A bare
        // fptosi/fptoui on such a source is LLVM undefined behaviour (poison;
        // on x86 it silently yields the integer-indefinite value), so guard the
        // conversion with an explicit in-range predicate first.
        //
        // The valid source interval is chosen so truncation-toward-zero lands
        // in [MIN, MAX]. We use the exactly-representable power-of-two bounds
        // (2^k is exact in both float and double for k ≤ 64): for a signed
        // N-bit target the interval is [-2^(N-1), 2^(N-1)); for unsigned it is
        // [0, 2^N). The upper bound is strict, so e.g. 2147483647.9 → int is
        // accepted (truncates to int.MaxValue) while 2147483648.0 overflows.
        // NaN fails both ordered comparisons and ±Infinity fails the matching
        // side, so all three error cases of §5.2 are rejected.
        auto pow2 = [](unsigned e) { double v = 1.0; for (unsigned i = 0; i < e; ++i) v *= 2.0; return v; };
        unsigned nbits = dstTy->getIntegerBitWidth();
        double maxBound = dstUns ? pow2(nbits) : pow2(nbits - 1);
        llvm::Value* maxC = llvm::ConstantFP::get(srcTy, maxBound);
        // Upper bound is exclusive (MAX+1 = 2^(N-1) or 2^N, an exact power of
        // two), so e.g. 2147483647.9 → int is accepted (truncates to int.MaxValue)
        // while 2147483648.0 overflows.
        llvm::Value* ltMax = builder_.CreateFCmpOLT(src, maxC, "f2i.ltmax");
        // Lower bound (§5.2 / #858-cluster): truncation TOWARD ZERO lands in
        // [MIN, MAX] for a source in the OPEN interval (MIN-1, MAX+1) — e.g.
        // sbyte(-128.9) truncates to -128 = sbyte.MinValue and must NOT raise.
        // The old `src >= MIN` wrongly rejected the (MIN-1, MIN] band. Use the
        // EXCLUSIVE lower bound `src > MIN-1` when MIN-1 is exactly representable
        // in the source FP type; when it is NOT (a large target whose MIN-1 rounds
        // to MIN — int←float, long←float/double), the FP granularity at that
        // magnitude is ≥ 1, so no source value lies strictly between MIN-1 and MIN
        // and `src >= MIN` is exact and correct. An integer is exact in float iff
        // |v| < 2^24, in double iff |v| < 2^53. Both forms are ORDERED comparisons
        // (false for NaN), so NaN/±Infinity stay rejected.
        unsigned srcMantBits = srcTy->isFloatTy() ? 24u : 53u;
        bool minMinus1Exact = dstUns || (nbits - 1) < srcMantBits;
        llvm::Value* geMin;
        if (minMinus1Exact) {
            double minExcl = (dstUns ? 0.0 : -pow2(nbits - 1)) - 1.0;
            geMin = builder_.CreateFCmpOGT(
                src, llvm::ConstantFP::get(srcTy, minExcl), "f2i.gtmin");
        } else {
            geMin = builder_.CreateFCmpOGE(
                src, llvm::ConstantFP::get(srcTy, -pow2(nbits - 1)), "f2i.gemin");
        }
        llvm::Value* inRange = builder_.CreateAnd(geMin, ltMax, "f2i.inrange");
        emitGuardThrow(builder_.CreateNot(inRange, "f2i.oor"), "OverflowException");
        return dstUns ? builder_.CreateFPToUI(src, dstTy)
                      : builder_.CreateFPToSI(src, dstTy);
    }
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
    // Defensive fallthrough. Same-width casts (incl. int↔uint signedness flips,
    // now CHECKED per §5.2/#753) return from the `srcTy == dstTy` branch above;
    // all genuine width changes are handled by the FP/int branches. Unreached.
    return src;
}


llvm::Value* Emitter::lowerEnumCast(const ast::CallExpr& call) {
    if (call.args.size() != 1 || !call.args[0].value) return nullptr;
    llvm::Value* src = lowerExpr(*call.args[0].value);
    if (!src || !src->getType()->isIntegerTy()) return nullptr;
    // §10.3 / §5.4 (#740): the enum is its underlying integer; width-adjust the
    // source integer to that width (default i32). Choose sign- vs zero-extension
    // by the SOURCE's signedness so a value from an unsigned source widens
    // without sign corruption.
    const sema::TypeSymbol* enumTy = nullptr;
    if (auto* rt = ctx_.exprType(call))
        if (rt->kind == sema::TypeRef::Kind::Named) enumTy = rt->declaration;
    llvm::Type* uTy = enumLlvmType(enumTy);
    bool srcUnsigned = false;
    if (auto* st = ctx_.exprType(*call.args[0].value))
        if (st->kind == sema::TypeRef::Kind::Primitive)
            srcUnsigned = isUnsignedPrim(st->primitive);
    // §5.4 (#859): at runtime `E(n)` raises OverflowException when `n` exceeds
    // the underlying type's range. On a NARROWING (source wider than the
    // underlying type) range-check the value using the UNDERLYING type's
    // signedness, mirroring the int→int narrowing guard: truncate, extend back
    // with the underlying signedness, and compare to the original — a mismatch
    // means the value did not fit. Without this `Color(300)` (byte-backed)
    // silently wrapped to 44 and `Color(-1)` to 255. A widening to the underlying
    // type cannot overflow, so it keeps the plain extension.
    unsigned srcBits = src->getType()->getIntegerBitWidth();
    unsigned dstBits = uTy->getIntegerBitWidth();
    if (dstBits < srcBits) {
        bool dstUns = enumIsUnsigned(enumTy);
        llvm::Value* trunc = builder_.CreateTrunc(src, uTy, "enum.cast.trunc");
        llvm::Value* back = dstUns
            ? builder_.CreateZExt(trunc, src->getType(), "enum.cast.back")
            : builder_.CreateSExt(trunc, src->getType(), "enum.cast.back");
        emitGuardThrow(builder_.CreateICmpNE(back, src, "enum.cast.oor"),
                       "OverflowException");
        return trunc;
    }
    return srcUnsigned ? builder_.CreateZExtOrTrunc(src, uTy, "enum.cast")
                       : builder_.CreateSExtOrTrunc(src, uTy, "enum.cast");
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
                // §9.1 / §14.1 (#769): resolve the field's type through the
                // active record generic substitution — for `Box<int>` with a
                // field `V: T?`, the declared `T?` must resolve to `int?` so the
                // nullable-value wrap below fires and the value is stored as the
                // tagged `{i8 hasValue; int value}` struct (the unresolved `T?`
                // was mis-judged, leaving the field wrong).
                const sema::TypeRef* fty = recordMemberTypeResolved(fields[i]);
                if (isNullableValueTy(fty)) {
                    auto* expectedSt = nullableValueStructTy(fty);
                    llvm::Type* valTy = expectedSt->getElementType(1);
                    if (v->getType() != expectedSt) {
                        if (v->getType()->isPointerTy() &&
                            llvm::isa<llvm::ConstantPointerNull>(v))
                            v = nullableValueNull(fty);
                        else if (v->getType() == valTy)
                            v = wrapNullableValue(fty, v);
                        else if (v->getType()->isIntegerTy() &&
                                 valTy->isIntegerTy())
                            // #743 / §9.1 / §5.1: a NARROW nullable value field
                            // (byte?/sbyte?/short?/ushort?) has an i8/i16 value
                            // half, but an int-typed argument (the literal `7`
                            // lowers to i32) does not match it. Coerce the
                            // integer to the value half's width before wrapping —
                            // without this the raw i32 was stored into the
                            // {i8, i8} slot, corrupting both the tag and value.
                            v = wrapNullableValue(
                                fty, builder_.CreateSExtOrTrunc(v, valTy));
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
    // §19 / §A (#727): a scrutinee-less `case cond => ... end` is an ordered
    // boolean-condition chain — each arm's "pattern" is a bool expression and
    // `_` is the catch-all — evaluated like an if/elif chain. It reuses the same
    // body/next/guard/result-PHI machinery; only the per-arm test is computed
    // differently (directly from the condition, not via patternMatches).
    llvm::Value* scrut = nullptr;
    const sema::TypeSymbol* enumTy = nullptr;
    bool isStr = false;
    const sema::TypeRef* scrutTy = nullptr;
    if (ce.scrutinee) {
        scrut = lowerExpr(*ce.scrutinee);
        if (!scrut) return nullptr;
        enumTy = enumTypeOf(*ce.scrutinee);
        isStr = isStringExprType(*ce.scrutinee);
        // §7 / issue #447: pass the scrutinee's resolved TypeRef so
        // patternMatches can detect a nullable VALUE type and apply
        // hasValue-based matching instead of the pointer-based paths.
        scrutTy = ctx_.exprType(*ce.scrutinee);
    }
    const sema::TypeRef* resultSemaTy = ctx_.exprType(ce);
    llvm::Type* resultTy = i32();
    if (resultSemaTy) resultTy = lowerTypeRef(resultSemaTy);

    auto* endBB = llvm::BasicBlock::Create(llvm_, "case.end", currentFn_);
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incomings;

    for (const auto& arm : ce.arms) {
        if (arm.bodyKind != ast::CaseArmBodyKind::Expression ||
            !arm.exprBody)
            return nullptr;  // block-body arms deferred
        llvm::Value* pm = nullptr;
        if (!ce.scrutinee) {
            // #727: scrutinee-less arm. `_` always matches; otherwise the arm's
            // pattern is one or more bool CONDITION expressions (comma = OR).
            if (dynamic_cast<const ast::DiscardPattern*>(arm.pattern.get())) {
                pm = builder_.getInt1(true);
            } else if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(
                           arm.pattern.get())) {
                for (const auto& c : cp->constants) {
                    if (!c) continue;
                    llvm::Value* cond = lowerExpr(*c);
                    if (!cond) continue;
                    if (!cond->getType()->isIntegerTy(1))
                        cond = builder_.CreateICmpNE(
                            cond, llvm::ConstantInt::get(cond->getType(), 0));
                    pm = pm ? builder_.CreateOr(pm, cond) : cond;
                }
            }
        } else {
            pm = patternMatches(arm.pattern.get(), scrut, enumTy, isStr, scrutTy);
        }
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
            // #738 / §19.5: a typed binding `var n: T` over a nullable VALUE
            // scrutinee `T?` binds the UNWRAPPED value (the {hasValue,T} struct's
            // T field), not the struct — the bound name has type T. (The match
            // condition, hasValue, is computed in patternMatches.) An
            // unconstrained `var x` keeps the whole nullable value.
            const sema::TypeRef* btyRef =
                bp->type ? ctx_.resolvedType(*bp->type) : nullptr;
            if (bp->type && scrutTy && isNullableValueTy(scrutTy) &&
                scrut->getType()->isStructTy()) {
                llvm::Value* inner = nullablePrimValue(scrut);
                llvm::AllocaInst* slot = entryAlloca(inner->getType());
                builder_.CreateStore(inner, slot);
                locals_[bp] = slot;
            } else if (btyRef && isBoxableValueType(btyRef) &&
                       scrut->getType()->isPointerTy()) {
                // #746 / §19.3 / §8.6: a typed binding `var n: T` where T is a
                // boxable VALUE type (int/bool/long/enum/record) over a BOXED
                // reference scrutinee (`Object`/`Object?` — `scrut` is a box
                // pointer) binds the UNBOXED value, not the raw box pointer.
                // Without this, `n` was the box ptr, so arithmetic/branch/field
                // access on it emitted ptr-vs-value IR (the IR_FAIL cluster) or
                // GEP'd off the scrutinee slot (the record WRONG_VALUE case).
                // The unbox is a payload deref (box + 2 words, like the `as`
                // unbox); guard it on `pm` (the isa-check), which is true only
                // when scrut is a non-null T — the binding store runs before the
                // match branch, so an unguarded deref would fault on a null/
                // mismatched scrutinee (a typed binding never matches null).
                llvm::Type* valTy = lowerTypeRef(btyRef);
                llvm::AllocaInst* slot = entryAlloca(valTy);
                builder_.CreateStore(llvm::Constant::getNullValue(valTy), slot);
                auto* ubBB = llvm::BasicBlock::Create(llvm_, "tb.unbox", currentFn_);
                auto* ubContBB =
                    llvm::BasicBlock::Create(llvm_, "tb.unbox.cont", currentFn_);
                builder_.CreateCondBr(pm, ubBB, ubContBB);
                builder_.SetInsertPoint(ubBB);
                llvm::Value* payload = builder_.CreateGEP(
                    ptrTy(), scrut, {builder_.getInt32(2)}, "tb.unbox.ptr");
                builder_.CreateStore(
                    builder_.CreateLoad(valTy, payload, "tb.unbox"), slot);
                builder_.CreateBr(ubContBB);
                builder_.SetInsertPoint(ubContBB);
                locals_[bp] = slot;
            } else {
                llvm::AllocaInst* slot = entryAlloca(scrut->getType());
                builder_.CreateStore(scrut, slot);
                locals_[bp] = slot;
            }
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
            // fix #677 / §9.4 / §19.2: an arm body `_ => this` inside a record
            // instance method returns `currentThis_` which is a POINTER to the
            // record struct (record methods receive `this` by pointer, §9.4/CG29),
            // but the case result type is the record struct by value. The PHI then
            // mixes struct-valued arms with a pointer arm → "PHI node operands are
            // not the same type" assertion in LLVM.
            //
            // Two cases to handle:
            //  A. sema annotated the case expression's result type and it is a
            //     struct (resultTy->isStructTy()): load through the ptr using that
            //     struct type. This covers the multi-arm case where a different arm
            //     already established the struct result type.
            //  B. sema did NOT annotate the case result (resultTy == i32 default):
            //     sema leaves `this` untyped for named-type methods (see
            //     ExprTypecheck_Exprs.cpp — `this` in a record method has no
            //     ctx_.exprType() → typeCase skips the arm → no common type → null
            //     exprType on the case). Detect this by checking v == currentThis_
            //     and looking up the current function in methodFns_ (same approach
            //     as lowerWith §9.5). Load through the record struct so the PHI is
            //     well-typed. This is the same load that lowerWith applies when
            //     `this with {…}` is used in a record method body.
            if (v->getType()->isPointerTy()) {
                if (resultTy && resultTy->isStructTy()) {
                    // Case A: sema-typed struct result — load through the ptr.
                    v = builder_.CreateLoad(resultTy, v, "rec.this.val");
                } else if (v == currentThis_ && currentFn_) {
                    // Case B: untyped arm, arm value IS this → look up the
                    // record struct from the emitter's method table.
                    llvm::StructType* recSt = nullptr;
                    for (const auto& [ms, fn] : methodFns_) {
                        if (fn == currentFn_ && ms->parent &&
                            ms->parent->kind == ast::TypeDeclKind::Record) {
                            recSt = recordStruct(ms->parent);
                            break;
                        }
                    }
                    if (!recSt && currentSpecMethodSymbol_ &&
                        currentSpecMethodSymbol_->parent &&
                        currentSpecMethodSymbol_->parent->kind ==
                            ast::TypeDeclKind::Record) {
                        recSt = recordStruct(currentSpecMethodSymbol_->parent);
                    }
                    if (recSt)
                        v = builder_.CreateLoad(recSt, v, "rec.this.val");
                }
            }
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

    // #836 / §19.3: a pattern CONSTANT or range/relop BOUND expression usually
    // has no resolved sema exprType (pattern exprs are not value-typechecked the
    // way ordinary expressions are), so its literal lowers at the DEFAULT width —
    // i32 for an integer literal, double for a float literal — regardless of the
    // scrutinee's actual width. Comparing such a value against a scrutinee of a
    // different width (a `long`/`ulong` i64, a `short`/`ushort` i16, an
    // `sbyte`/`byte` i8, or a `float` f32) made CreateICmp*/CreateFCmp* assert on
    // mismatched operand types — a hard compiler abort. `reconcile` brings a
    // pattern value to the scrutinee's LLVM type before the comparison: integer
    // trunc / (z|s)ext by the scrutinee's signedness, FP ext/trunc.
    bool scrutUnsigned = scrutTy &&
                         scrutTy->kind == sema::TypeRef::Kind::Primitive &&
                         isUnsignedPrim(scrutTy->primitive);
    auto reconcile = [&](llvm::Value* v, llvm::Type* targetTy) -> llvm::Value* {
        if (!v || !targetTy) return v;
        llvm::Type* st = v->getType();
        if (st == targetTy) return v;
        if (st->isIntegerTy() && !st->isIntegerTy(1) &&
            targetTy->isIntegerTy() && !targetTy->isIntegerTy(1)) {
            unsigned sw = st->getIntegerBitWidth();
            unsigned tw = targetTy->getIntegerBitWidth();
            if (sw == tw) return v;
            if (sw > tw) return builder_.CreateTrunc(v, targetTy);
            return scrutUnsigned ? builder_.CreateZExt(v, targetTy)
                                 : builder_.CreateSExt(v, targetTy);
        }
        if (st->isFloatingPointTy() && targetTy->isFloatingPointTy()) {
            unsigned sw = st->getPrimitiveSizeInBits();
            unsigned tw = targetTy->getPrimitiveSizeInBits();
            if (sw < tw) return builder_.CreateFPExt(v, targetTy);
            if (sw > tw) return builder_.CreateFPTrunc(v, targetTy);
        }
        return v;
    };
    // #836: lower an integer-literal pattern constant DIRECTLY at the
    // scrutinee's width. The general lowerExpr path defaults an untyped integer
    // literal to i32 and builds the ConstantInt at that width, which TRUNCATES a
    // value exceeding i32 (e.g. a `long` constant `5000000000L`, or a `ulong`
    // constant above 2^63) before `reconcile` could widen it — yielding a wrong
    // match. Building the constant at the target width up front preserves the
    // full value. Returns null for any expr that is not an integer literal (or a
    // negated one), so the caller falls back to lowerExpr + reconcile.
    auto litAtWidth = [&](const ast::Expr& c,
                          llvm::Type* wantTy) -> llvm::Value* {
        if (!wantTy || !wantTy->isIntegerTy() || wantTy->isIntegerTy(1))
            return nullptr;
        const ast::Expr* e = &c;
        bool neg = false;
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(e))
            if (un->op == "-" && un->operand) { neg = true; e = un->operand.get(); }
        auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(e);
        if (!lit) return nullptr;
        std::uint64_t mag =
            static_cast<std::uint64_t>(parseIntLiteral(lit->text));
        std::uint64_t bits = neg ? (static_cast<std::uint64_t>(0) - mag) : mag;
        return llvm::ConstantInt::get(wantTy, bits, /*isSigned=*/true);
    };

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
                llvm::Value* cv = litAtWidth(*c, innerVal->getType());  // #836
                if (!cv) cv = reconcile(lowerExpr(*c), innerVal->getType());
                if (!cv) cv = enumConstantOfPatternExpr(*c, enumTy);
                if (!cv) return nullptr;
                llvm::Value* eq = emitEquals(innerVal, cv, /*isStr=*/false);
                llvm::Value* match = builder_.CreateAnd(hasVal, eq, "nv.val.match");
                any = builder_.CreateOr(any, match);
            }
            return any;
        }

        // §19.3 / §19.5 (#738): a typed BindingPattern `var n: T` on a nullable
        // value `T?` matches only when the value is present (hasValue) — §19.5:
        // typed bindings never match null. The bound name is the UNWRAPPED value
        // (lowerCaseExpr stores innerVal into the value-typed slot). An
        // unconstrained `var x` binds the whole nullable value (incl. null) and
        // always matches.
        if (auto* bp = dynamic_cast<const ast::BindingPattern*>(pat)) {
            if (bp->type) return hasVal;
            return builder_.getInt1(true);
        }

        // §19.3 (#738): a RangePattern on a nullable value — hasValue AND the
        // unwrapped value is in range. Mirrors the non-nullable range logic
        // below, applied to innerVal. (Open-ended forms: an absent bound is
        // vacuously satisfied.)
        if (auto* rp = dynamic_cast<const ast::RangePattern*>(pat)) {
            auto* range =
                dynamic_cast<const ast::RangeExpr*>(rp->rangeExpr.get());
            if (range && !range->step && (range->lo || range->hi)) {
                bool fp = innerVal->getType()->isFloatingPointTy();
                llvm::Value* geL = builder_.getInt1(true);
                if (range->lo) {
                    llvm::Value* lo = litAtWidth(*range->lo, innerVal->getType());
                    if (!lo) lo = reconcile(lowerExpr(*range->lo),
                                            innerVal->getType());  // #836
                    if (!lo) return nullptr;
                    geL = fp ? builder_.CreateFCmpOGE(innerVal, lo)
                             : (scrutUnsigned ? builder_.CreateICmpUGE(innerVal, lo)
                                              : builder_.CreateICmpSGE(innerVal, lo));
                }
                llvm::Value* leH = builder_.getInt1(true);
                if (range->hi) {
                    llvm::Value* hi = litAtWidth(*range->hi, innerVal->getType());
                    if (!hi) hi = reconcile(lowerExpr(*range->hi),
                                            innerVal->getType());  // #836
                    if (!hi) return nullptr;
                    leH = fp
                        ? (range->inclusive ? builder_.CreateFCmpOLE(innerVal, hi)
                                            : builder_.CreateFCmpOLT(innerVal, hi))
                        : (range->inclusive
                               ? (scrutUnsigned ? builder_.CreateICmpULE(innerVal, hi)
                                                : builder_.CreateICmpSLE(innerVal, hi))
                               : (scrutUnsigned ? builder_.CreateICmpULT(innerVal, hi)
                                                : builder_.CreateICmpSLT(innerVal, hi)));
                }
                llvm::Value* inRange = builder_.CreateAnd(geL, leH);
                return builder_.CreateAnd(hasVal, inRange, "nv.range.match");
            }
            return nullptr;
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
    if (dynamic_cast<const ast::TypePattern*>(pat)) {
        // §19.5 (#760): "A bare type pattern T matches only non-null values."
        // A nullable runtime-managed-reference scrutinee (`string?`, a function
        // type, a boxed value) is a *pointer*, not the tagged struct the
        // isNullableVal branch handles — `isNullableValueTy` is false for it, so
        // it falls through here. Its runtime value may be a null pointer, which
        // the bare type pattern must NOT match (only `null` / `_` / untyped
        // `var x` do). Emit the non-null requirement; a non-nullable scrutinee
        // is statically never null and stays an unconditional match.
        if (scrutTy && scrutTy->nullable && scrut->getType()->isPointerTy())
            return builder_.CreateICmpNE(
                scrut,
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(scrut->getType())),
                "typepat.nonnull");
        return builder_.getInt1(true);
    }
    if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(pat)) {
        llvm::Value* any = builder_.getInt1(false);
        for (const auto& c : cp->constants) {
            if (!c) continue;
            // #836: lower an integer-literal constant at the scrutinee's width
            // up front (avoids the default-i32 truncation of a wide value).
            llvm::Value* cv = isStr ? nullptr : litAtWidth(*c, scrut->getType());
            if (!cv) cv = lowerExpr(*c);
            // CG43: an enum-constant alternative (`E.Red`, `E.Green`) has
            // no resolved symbol on the pattern expression, so lowerExpr
            // misses; resolve it by name against the scrutinee's enum.
            if (!cv) cv = enumConstantOfPatternExpr(*c, enumTy);
            if (!cv) return nullptr;
            // #836: reconcile a non-literal constant's width to the scrutinee
            // (no-op for string / enum / already-matching types).
            if (!isStr) cv = reconcile(cv, scrut->getType());
            // CG27b: string scrutinee → content comparison (not ptr cmp).
            any = builder_.CreateOr(any, emitEquals(scrut, cv, isStr));
        }
        return any;
    }
    if (auto* rp = dynamic_cast<const ast::RelOpPattern*>(pat)) {
        llvm::Value* rv =
            rp->expr ? litAtWidth(*rp->expr, scrut->getType()) : nullptr;  // #836
        if (!rv && rp->expr) rv = reconcile(lowerExpr(*rp->expr), scrut->getType());
        if (!rv) return nullptr;
        bool fp = scrut->getType()->isFloatingPointTy();
        if (rp->op == "<")
            return fp ? builder_.CreateFCmpOLT(scrut, rv)
                      : (scrutUnsigned ? builder_.CreateICmpULT(scrut, rv)
                                       : builder_.CreateICmpSLT(scrut, rv));
        if (rp->op == ">")
            return fp ? builder_.CreateFCmpOGT(scrut, rv)
                      : (scrutUnsigned ? builder_.CreateICmpUGT(scrut, rv)
                                       : builder_.CreateICmpSGT(scrut, rv));
        if (rp->op == "<=")
            return fp ? builder_.CreateFCmpOLE(scrut, rv)
                      : (scrutUnsigned ? builder_.CreateICmpULE(scrut, rv)
                                       : builder_.CreateICmpSLE(scrut, rv));
        if (rp->op == ">=")
            return fp ? builder_.CreateFCmpOGE(scrut, rv)
                      : (scrutUnsigned ? builder_.CreateICmpUGE(scrut, rv)
                                       : builder_.CreateICmpSGE(scrut, rv));
    }
    // §19: range patterns `lo..hi` and `lo..<hi` — same semantics as
    // the `in` operator but in a case arm. The pattern matches when
    // lo <= scrut <= hi (inclusive) or lo <= scrut < hi (half-open).
    if (auto* rp = dynamic_cast<const ast::RangePattern*>(pat)) {
        auto* range = dynamic_cast<const ast::RangeExpr*>(
            rp->rangeExpr.get());
        // §16.2 / §19.3 (#726): support OPEN-ENDED range patterns (`31..`,
        // `..<10`, `..b`) in addition to closed `a..b`/`a..<b`. One bound may be
        // absent: an absent lower bound matches everything below the upper
        // (geL = true), an absent upper bound matches everything at/above the
        // lower (leH = true). A `..`-only pattern reduces to true.
        if (range && !range->step && (range->lo || range->hi)) {
            bool fp = scrut->getType()->isFloatingPointTy();
            // §19.3: range patterns — use FP comparisons for float scrutinees.
            llvm::Value* geL = builder_.getInt1(true);
            if (range->lo) {
                llvm::Value* lo = litAtWidth(*range->lo, scrut->getType());  // #836
                if (!lo) lo = reconcile(lowerExpr(*range->lo), scrut->getType());
                if (!lo) return nullptr;
                geL = fp ? builder_.CreateFCmpOGE(scrut, lo)
                         : (scrutUnsigned ? builder_.CreateICmpUGE(scrut, lo)
                                          : builder_.CreateICmpSGE(scrut, lo));
            }
            llvm::Value* leH = builder_.getInt1(true);
            if (range->hi) {
                llvm::Value* hi = litAtWidth(*range->hi, scrut->getType());  // #836
                if (!hi) hi = reconcile(lowerExpr(*range->hi), scrut->getType());
                if (!hi) return nullptr;
                leH = fp
                    ? (range->inclusive ? builder_.CreateFCmpOLE(scrut, hi)
                                        : builder_.CreateFCmpOLT(scrut, hi))
                    : (range->inclusive
                           ? (scrutUnsigned ? builder_.CreateICmpULE(scrut, hi)
                                            : builder_.CreateICmpSLE(scrut, hi))
                           : (scrutUnsigned ? builder_.CreateICmpULT(scrut, hi)
                                            : builder_.CreateICmpSLT(scrut, hi)));
            }
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
                    // §9.6 / §10: a RECORD or ENUM is a sealed VALUE type with no
                    // subtyping, so a typed binding `var q: R` over an UNBOXED
                    // such scrutinee is a STATIC test — the scrutinee's static
                    // type either IS R or is not. An unboxed record scrutinee is a
                    // struct VALUE and an enum is an i32 (neither a pointer), so
                    // the runtime is-a path (emitIsA, which walks a vtable
                    // pointer) emitted an ICmp on mismatched operand types and
                    // crashed the backend (#801: discovery probe). Decide
                    // statically. A BOXED record/enum (scrutinee `Object`/
                    // interface — a pointer) is genuinely a runtime type test and
                    // falls through to emitIsA / the box-descriptor path below
                    // (the §19.5 unbox case, #746).
                    auto declKind = rt->declaration->kind;
                    if ((declKind == ast::TypeDeclKind::Record ||
                         declKind == ast::TypeDeclKind::Enum) &&
                        !scrut->getType()->isPointerTy()) {
                        bool sameType =
                            scrutTy && scrutTy->declaration == rt->declaration;
                        return builder_.getInt1(sameType);
                    }
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
                if (rt->kind == sema::TypeRef::Kind::Primitive) {
                    bool scrutIsSamePrim =
                        scrutTy &&
                        scrutTy->kind == sema::TypeRef::Kind::Primitive &&
                        scrutTy->primitive == rt->primitive && !scrutTy->nullable;
                    // §19.5 (#856): `var n: string` against a string scrutinee
                    // that is NOT the statically-present non-nullable case — a
                    // `string?` (a possibly-null pointer, NOT the {i8,T} tagged
                    // struct, so it never reached the nullable-VALUE branch above)
                    // or a boxed `Object`/`Object?` holding a string. It fell into
                    // the boxed-primitive path below, whose `bz_obj_is_string`
                    // short-circuit returns FALSE for a real string (correct for
                    // `var n: int` — a string is not an int — but WRONG for
                    // `var n: string`). A non-null `string?` IS a `string`. Match
                    // iff the value is a non-null string.
                    if (!scrutIsSamePrim &&
                        rt->primitive == sema::PrimitiveType::String &&
                        scrut->getType()->isPointerTy()) {
                        llvm::Value* nonNull = builder_.CreateICmpNE(
                            scrut, llvm::ConstantPointerNull::get(ptrTy()),
                            "tb.str.nonnull");
                        // A statically `string`/`string?` scrutinee: a non-null
                        // value is necessarily a string, so non-null is the test.
                        if (scrutTy &&
                            scrutTy->kind == sema::TypeRef::Kind::Primitive &&
                            scrutTy->primitive == sema::PrimitiveType::String)
                            return nonNull;
                        // An `Object`/`Object?` scrutinee: a runtime is-string
                        // test, guarded by non-null (bz_obj_is_string on null is
                        // not called).
                        llvm::BasicBlock* preBB = builder_.GetInsertBlock();
                        auto* isStrFn = declareC("bz_obj_is_string", i32(),
                                                 {ptrTy(), ptrTy()});
                        auto* chkBB = llvm::BasicBlock::Create(
                            llvm_, "tb.str.chk", currentFn_);
                        auto* joinBB = llvm::BasicBlock::Create(
                            llvm_, "tb.str.join", currentFn_);
                        builder_.CreateCondBr(nonNull, chkBB, joinBB);
                        builder_.SetInsertPoint(chkBB);
                        llvm::Value* isStr = builder_.CreateICmpNE(
                            builder_.CreateCall(isStrFn,
                                                {scrut, vtableTablePtr()}),
                            builder_.getInt32(0));
                        llvm::BasicBlock* chkEnd = builder_.GetInsertBlock();
                        builder_.CreateBr(joinBB);
                        builder_.SetInsertPoint(joinBB);
                        auto* phi = builder_.CreatePHI(i1(), 2);
                        phi->addIncoming(builder_.getInt1(false), preBB);
                        phi->addIncoming(isStr, chkEnd);
                        return phi;
                    }
                    // #746 / §19.3/§19.5: when the scrutinee is a BOXED reference
                    // (`Object`/`Object?` — `scrut` is a box pointer, not the
                    // primitive itself), the typed binding is a RUNTIME type-test
                    // against the boxed primitive's descriptor, NOT an
                    // unconditional match. Without this, `var n: int` matched ANY
                    // boxed value (a string, a null) and the arm then unboxed
                    // garbage / dereferenced null.
                    if (!scrutIsSamePrim && scrut->getType()->isPointerTy())
                        if (llvm::GlobalVariable* desc = boxDescriptorFor(rt)) {
                            // A string in an Object is a vtable-less bz_string*;
                            // emitIsA would fault walking it. A string is never a
                            // boxed primitive, so short-circuit to false. (Guard
                            // localized here, not in emitIsA: a cross-module class
                            // mis-seen as a string by bz_obj_is_string still
                            // yields the correct false for a primitive test — it
                            // is not this primitive either way — so this does not
                            // disturb exception/class dispatch that calls emitIsA
                            // directly.)
                            llvm::BasicBlock* preBB = builder_.GetInsertBlock();
                            auto* isStrFn = declareC("bz_obj_is_string", i32(),
                                                     {ptrTy(), ptrTy()});
                            llvm::Value* isStr = builder_.CreateICmpNE(
                                builder_.CreateCall(
                                    isStrFn, {scrut, vtableTablePtr()}),
                                builder_.getInt32(0));
                            auto* doIsaBB = llvm::BasicBlock::Create(
                                llvm_, "tb.isa", currentFn_);
                            auto* joinBB = llvm::BasicBlock::Create(
                                llvm_, "tb.isa.join", currentFn_);
                            builder_.CreateCondBr(isStr, joinBB, doIsaBB);
                            builder_.SetInsertPoint(doIsaBB);
                            llvm::Value* isa = emitIsA(scrut, desc);
                            llvm::BasicBlock* isaEnd = builder_.GetInsertBlock();
                            builder_.CreateBr(joinBB);
                            builder_.SetInsertPoint(joinBB);
                            auto* phi = builder_.CreatePHI(i1(), 2);
                            phi->addIncoming(builder_.getInt1(false), preBB);
                            phi->addIncoming(isa, isaEnd);
                            return phi;
                        }
                    return builder_.getInt1(true);
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
