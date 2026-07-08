#include "codegen/LlvmBackend_impl.hpp"

// This translation unit continues the definitions of Emitter's expression-lowering
// member functions begun in LlvmBackend_Exprs.cpp.  It was split off mechanically
// (definition-motion only, no behavior change; #1414 stage 5-replacement) to halve
// that ~12.7k-line TU.  The Emitter class is declared in LlvmBackend_impl.hpp and
// its declarations are unchanged; only definitions moved here.
//
// §4.4 fix #521: forward declaration of the runtime decimal parser, also declared
// in LlvmBackend_Exprs.cpp.  bz_decimal_parse_cstr is compiled into the same
// process and safe to call during code generation (both AOT and JIT).  It is
// duplicated here because a decimal-literal pattern in patternMatches (below)
// calls it. // per fix #521
extern "C" void bz_decimal_parse_cstr(const char* s, void* out,
                                       void* ove_vtable, void* fe_vtable);

namespace bezel::codegen {

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
                // #1104: pre-build the nullable-wrapped form of the (non-null) LHS
                // payload HERE in entryBB (so it dominates the PHI's entry edge),
                // for the case the RHS is also a nullable value (`int? ?? int?`).
                // There the result is `T?` and both PHI incomings must be the
                // `{i8 hasValue; T}` struct — previously the PHI used the bare-T
                // payload type while the RHS was `{i8,T}` → an LLVM PHI same-type
                // assertion (SIGABRT). The wrap is dead (DCE'd) when the result is
                // non-nullable (`int? ?? 5`).
                llvm::Type* coalNullableTy = llvm::StructType::get(
                    llvm_, {nullablePrimHasValTy(), primVal->getType()});
                llvm::Value* wrappedLhs = builder_.CreateInsertValue(
                    llvm::UndefValue::get(coalNullableTy),
                    llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
                wrappedLhs = builder_.CreateInsertValue(wrappedLhs, primVal,
                                                        {1u});
                builder_.CreateCondBr(hasVal, endBB, rhsBB);
                builder_.SetInsertPoint(rhsBB);
                llvm::Value* r = lowerExpr(*bin.right);
                bool rhsDiv = diverged_;
                if (!r) return nullptr;
                // #1044 / #1046: coerce the RHS default to the nullable payload
                // type HERE — in rhsBB, BEFORE the branch to endBB — so the
                // coerced value dominates the PHI edge. (Doing it at endBB worked
                // only for a CONSTANT default, which folds to a ConstantExpr with
                // no instruction; a VARIABLE default `x ?? v` emitted a real
                // instruction in endBB that did not dominate rhsEndBB → an LLVM
                // "instruction does not dominate all uses" verifier failure.) The
                // RHS is lowered at its own natural width/type:
                //  - integer payload of a different width (sub-i32 `byte?`/...,
                //    or wider `long?`): int-cast the RHS to the payload width.
                //  - decimal payload (`decimal? ?? <int>`): widen the int RHS to a
                //    decimal struct (§5.1 int→decimal) — without this the PHI
                //    mixed a %bz.decimal payload with an i32 default and asserted.
                if (!rhsDiv && r->getType()->isIntegerTy()) {
                    if (primVal->getType()->isIntegerTy() &&
                        primVal->getType() != r->getType()) {
                        r = builder_.CreateIntCast(r, primVal->getType(),
                                                   /*isSigned=*/true);
                    } else if (primVal->getType() == decimalStructType()) {
                        const sema::TypeRef* rt = ctx_.exprType(*bin.right);
                        sema::PrimitiveType rp =
                            (rt && rt->kind == sema::TypeRef::Kind::Primitive)
                                ? rt->primitive : sema::PrimitiveType::Int;
                        llvm::AllocaInst* dslot = entryAlloca(decimalStructType());
                        emitIntToDecimal(r, rp, dslot);
                        r = builder_.CreateLoad(decimalStructType(), dslot,
                                                "coal.dec.rhs");
                    }
                }
                llvm::BasicBlock* rhsEndBB = nullptr;
                if (!rhsDiv) { rhsEndBB = builder_.GetInsertBlock(); builder_.CreateBr(endBB); }
                builder_.SetInsertPoint(endBB);
                diverged_ = false;
                if (rhsDiv) return primVal;
                // #1104: when the RHS is itself a nullable value (its lowered
                // type is the same `{i8,T}` struct), the `??` result is `T?` —
                // wrap the LHS payload so both incomings match. Otherwise the
                // result is the bare payload `T` (the common `int? ?? 5` case).
                bool resultNullable = r->getType() == coalNullableTy;
                auto* phi = builder_.CreatePHI(
                    resultNullable ? coalNullableTy : primVal->getType(), 2);
                phi->addIncoming(resultNullable ? wrappedLhs : primVal, entryBB);
                phi->addIncoming(r, rhsEndBB);
                return phi;
            }
        }
        // #1045: a non-pointer, non-nullable-value LHS — `nonNullable ?? rhs`.
        // The left operand can never be null (a value type that is not the
        // {tag,payload} nullable form, e.g. a lifted relational result which is
        // a definite `bool` per §7.11 "false when either operand is null"), so
        // the result is always the left operand and the rhs is dead. Returning
        // it (rather than nullptr) fixes `(int? < 5) ?? false`, which lowered the
        // whole `??` to null → false. The rhs is intentionally NOT lowered here
        // (correct short-circuit: a non-null left never evaluates the default).
        return l;
    }
    auto* entryBB = builder_.GetInsertBlock();
    auto* rhsBB = llvm::BasicBlock::Create(llvm_, "coalesce.rhs", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "coalesce.end", currentFn_);
    llvm::Value* isNull = builder_.CreateICmpEQ(
        l, llvm::ConstantPointerNull::get(ptrTy()));
    // #1029 / §28.3: when the RHS is a fresh owned-class producer (so the ?? result
    // is owned by the binding variable), transfer ownership from the LHS slot to the
    // result by nulling the LHS slot HERE (before the branch) so that the LHS's own
    // null-guarded scope-end disposal is a no-op.  This prevents a double-dispose:
    //   • Non-null path: l is already in a register; storing null to the LHS slot
    //     does not affect l, but the LHS variable's disposal (if any) sees null and
    //     skips — the binding's disposal runs instead (exactly once).
    //   • Null path: the LHS slot was already null; the null-store is a no-op.
    // Skipped for captured (cell-boxed) locals because their alloca is a cell-HOLDER
    // pointer — nulling it would corrupt cell tracking; those cases remain a
    // potential double-dispose until a separate owning-capture fix. // per §28.3
    if (bin.right && isFreshOwnedClassProducer(bin.right.get())) {
        if (llvm::AllocaInst* lhsSlot = localSlot(*bin.left)) {
            if (!cellVars_.count(lhsSlot))
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), lhsSlot);
        }
    }
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
            // #1219 (per #1059 / §25.2 / §14): a unary operator overload
            // (+/-/~) on a GENERIC record uses the declaring type's T in its
            // parameter/return types.  Set up the per-instantiation
            // substitution from the operand's concrete generic args so
            // emitMethodCall emits the specialized form.  Without this the
            // unspecialized function (record.Box) is called with a
            // specialized-struct (record.Box$spec0) argument → IR_FAIL.
            auto savedOpSubst = genericSubst_;
            auto savedOpSema  = genericSubstSemaTy_;
            const sema::TypeRef* opTy = ctx_.exprType(*un.operand);
            if (opTy && opTy->kind == sema::TypeRef::Kind::Named &&
                opTy->declaration == opm->parent &&
                !opTy->genericArgs.empty()) {
                auto* td = dynamic_cast<const ast::TypeDecl*>(
                    opTy->declaration->decl);
                if (td && td->genericParams.size() ==
                               opTy->genericArgs.size()) {
                    std::unordered_map<std::string_view, llvm::Type*> ns;
                    std::unordered_map<std::string_view,
                        const sema::TypeRef*> nsem;
                    for (std::size_t gi = 0;
                         gi < td->genericParams.size(); ++gi) {
                        ns[td->genericParams[gi].name] =
                            lowerTypeRef(opTy->genericArgs[gi]);
                        nsem[td->genericParams[gi].name] =
                            resolveTypeParamRef(opTy->genericArgs[gi]);
                    }
                    genericSubst_       = std::move(ns);
                    genericSubstSemaTy_ = std::move(nsem);
                }
            }
            llvm::Value* result =
                emitMethodCall(opm, args, /*baseQualified=*/false);
            genericSubst_       = std::move(savedOpSubst);
            genericSubstSemaTy_ = std::move(savedOpSema);
            return result;
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
        // §28.8.1 / #1328 merge gate: `move _f!` / `move this._f!` — a
        // null-assert wrapper does not change WHICH slot is moved from; unwrap
        // it so the slot-nulling form matching below sees the underlying
        // field/local form. Without this the field slot kept the moved-out
        // pointer, and the subsequent `_f := null` (now a §28.7 drop-on-
        // overwrite site) disposed the value the new owner also disposes — a
        // double-free (undisposed_leak kMoveField, caught by the serial suite).
        const ast::Expr* moveSrc = un.operand.get();
        while (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(moveSrc))
            moveSrc = pb->operand.get();
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(moveSrc)) {
            sema::Symbol* sym = ctx_.exprSymbol(*id);
            if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
                // Null the source slot when: (a) it is a `using var` local whose
                // scope-end disposal must be suppressed (existing rule), OR (b) it is
                // an owned iteration variable flagged for per-iteration auto-drop
                // (#1329 / §29.2/§28.3 v0.157): the auto-drop at body-end is
                // null-guarded, so nulling here prevents a double-dispose when the
                // object is moved to a new owner that disposes it.
                const bool isIterVarAutoDrop = [&]() -> bool {
                    if (auto* fs_node =
                            dynamic_cast<const ast::ForStmt*>(loc->decl))
                        return fs_node->ownedIterVarAutoDropAtBodyEnd;
                    return false;
                }();
                // (c) §28.3 / §28.11 (#1466): an out-owned recipient (inline
                // `out owned var` or a pre-declared `out item`) whose scope-end
                // reclaim the ownership pass armed as a null-guarded auto-drop
                // (LocalSymbol::autoDropAtScopeEnd → dynamicDispose in
                // ensureOutVarSlots). Nulling the slot on `move` keeps that
                // reclaim a no-op after the value is transferred to a new owner —
                // exactly-once-safe under a conditional discharge, the same
                // reasoning as the iter-var case. (autoDropAtScopeEnd on a
                // LocalSymbol is set ONLY for out-owned recipients — a regular
                // plain `var`'s auto-drop lives on its LocalDecl — so this does
                // not disturb the plain-`var` borrow-after-move idiom.)
                if (loc->isUsing || isIterVarAutoDrop ||
                    loc->autoDropAtScopeEnd) {
                    if (llvm::AllocaInst* slotAI = localSlot(*id)) {
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
                    if (llvm::Value* addr = implicitFieldAddr(*id, fty)) {
                        if (fty && fty->isPointerTy())
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), addr);
                    }
                }
            }
        } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                       moveSrc)) {
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
            } else if (par && par->resolvedDeclType) {
                // #1124: an inferred (un-annotated) lambda parameter whose type
                // sema resolved from context — e.g. an `Aggregate(seed, (acc, x)
                // => …)` fold's `acc: TAccum`, seeded from the seed argument's
                // type. Honor it so a non-element accumulator (a `string`
                // accumulator, say) is lowered with its true LLVM type instead of
                // the i32 default (which mismatched the caller's accumulator type
                // and emitted malformed IR). For an element-typed inferred param
                // (the common single-param LINQ case) this yields the same type
                // the expected-function-type fallback below would.
                ty = lowerTypeRef(par->resolvedDeclType);
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
                    auto gsit = genericSubst_.find(expRT->headName());
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
                                expRT->headName());
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
                auto sit = genericSubst_.find(rt->headName());
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
        // §30 / #1377: same smAllocaToField_ update as for local captures above
        // — a captured param of an async method that is a suspend-local needs its
        // cell holder tracked for correct spill/restore across await points.
        if (newSlot != slot) {
            auto smit = smAllocaToField_.find(slot);
            if (smit != smAllocaToField_.end()) {
                unsigned fieldIdx = smit->second;
                smAllocaToField_.erase(smit);
                smAllocaToField_[newSlot] = fieldIdx;
            }
        }
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
    // #1207 / §27.5/§22.3: captured outer ref/out params.
    // A ref/out param lives in refParams_ (a pointer to the caller's storage),
    // not paramSlots_, so the paramSlots_ loop above silently skips it.
    // We capture the caller's pointer directly: the env field holds it, and
    // inside the lambda the restored ptr-alloca is registered in cellVars_ with
    // the param's element type, so boxedStorage routes reads/writes through the
    // caller's storage — aliasing the caller's variable exactly as §27.5 requires
    // ("modifications inside the lambda are visible outside, and vice versa").
    // No heap cell is allocated; no RC obligation arises (the ref param is a
    // borrow of the caller's storage, not owned by this method or the lambda).
    std::vector<const sema::Symbol*> capturedRefParamSyms;
    for (auto& [sym, addrElem] : refParams_) {
        if (capturedSyms.find(sym) != capturedSyms.end())
            capturedRefParamSyms.push_back(sym);
    }
    for (const sema::Symbol* sym : capturedRefParamSyms) {
        auto& [callerPtr, elemTy] = refParams_[sym];
        CaptureEntry ce;
        ce.valTy    = elemTy;    // the value type behind the pointer
        ce.envValue = callerPtr; // the pointer itself goes into the env field
        ce.sym      = sym;
        ce.decl     = nullptr;
        // semaTy / releaseBoxInEnvdtor / freeCellInEnvdtor all stay at their
        // zero-initialised defaults: the closure holds no ownership over the
        // caller's storage, so the envdtor must not release or free anything.
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
        // §30 / #1377: when the enclosing function is an ASYNC STATE MACHINE and
        // this captured local is a suspend-local (tracked in smAllocaToField_ for
        // spill/restore across await yield points), update smAllocaToField_ to
        // track the CELL HOLDER (newSlot) instead of the original slot.  The cell
        // holder holds the cell_ptr (a heap pointer); after boxing, the yield
        // spill must save cell_ptr and the post-await restore must reload it into
        // newSlot so that body code (which reads box through newSlot) sees the
        // correct cell.  Without this update, newSlot is uninitialized on the
        // post-await resume path (the original slot's frame field restores Box*
        // into the stale slot, but newSlot is never filled → null/garbage cell).
        if (newSlot != slot) {
            auto smit = smAllocaToField_.find(slot);
            if (smit != smAllocaToField_.end()) {
                unsigned fieldIdx = smit->second;
                smAllocaToField_.erase(smit);
                smAllocaToField_[newSlot] = fieldIdx;
            }
        }
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
        // §27.5 / §28.9 / #672: string and closure (function-typed) captures need
        // the SAME additional-capture retain as boxes (above). Each capturing env's
        // envdtor releases the captured string/closure unconditionally on the sema
        // type, while the suppressed scope-end release covers only ONE reference. So
        // the FIRST capture transfers that single suppressed release; every
        // ADDITIONAL (alreadyBoxed) capture must mint an independent +1 here — else
        // two closures capturing one string both release the shared cell's string →
        // over-release / heap-use-after-free (#672). N captures ⇒ 1 (suppressed
        // producer) + (N-1) retains = N references = N envdtor releases → 0 after the
        // last env dies. Exclude guarded boxing (loop-iteration cell reuse) for the
        // same reason the box branch does (conservative leak, never over-release).
        if (alreadyBoxed && !wasGuardedLocal) {
            if (isStringTypeRefNonNull(ce.semaTy)) {
                llvm::Value* str =
                    builder_.CreateLoad(ptrTy(), ce.envValue, "str.recap");
                emitStringRetain(str);
            } else if (ce.semaTy &&
                       ce.semaTy->kind == sema::TypeRef::Kind::Function) {
                llvm::Value* cl =
                    builder_.CreateLoad(ptrTy(), ce.envValue, "cl.recap");
                emitClosureRetain(cl);
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
            // T null; `Task<T>` / `OwningTask<T>` carries the element type T.
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
            // §27.4 (v0.183, #1408): a destructuring lambda parameter has no
            // single name to bind — instead of storing the whole incoming
            // record argument into one paramSlots_ entry, extract each
            // destructured slot's field directly (mirrors the case-pattern
            // DestructuringPattern prologue, patternMatches() above, but
            // unconditional — sema already fixed the parameter's exact type).
            if (par && par->destructure) {
                bindLambdaDestructureParam(*par->destructure, &*it);
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
        // emitAsyncLambda recomputes closure-release AND capture-cell-free
        // eligibility for the LAMBDA's own body and keys them to the lambda resume
        // fn. The ENCLOSING function's eligibility (which governs whether THIS
        // `var f := <lambda>` local gets its scope-end closure release / capture-
        // cell free — registered AFTER lowerLambda returns, and consulted again by
        // any SUBSEQUENT lambda in the enclosing body) must be preserved; save and
        // restore all of it across the call. (#1416/#1418: the capture-cell sets
        // are now recomputed inside emitAsyncLambda too, so they need the same
        // save/restore the closure-release sets already had.)
        auto savedClosureRelElig = closureReleaseEligible_;
        llvm::Function* savedClosureRelEligFn = closureReleaseEligibleFn_;
        auto savedCellFreeElig = cellFreeEligible_;
        llvm::Function* savedCellFreeEligFn = cellFreeEligibleFn_;
        auto savedBoxCaptureLocals = boxCaptureLocals_;
        auto savedSingleCaptureLocals = singleCaptureLocals_;
        llvm::Function* savedBoxCaptureLocalsFn = boxCaptureLocalsFn_;
        auto savedDefinitelyEscaping = definitelyEscapingLambdas_;

        emitAsyncLambda(lam, fn, paramSyms, asyncResultElemSemaTy);

        closureReleaseEligible_ = std::move(savedClosureRelElig);
        closureReleaseEligibleFn_ = savedClosureRelEligFn;
        cellFreeEligible_ = std::move(savedCellFreeElig);
        cellFreeEligibleFn_ = savedCellFreeEligFn;
        boxCaptureLocals_ = std::move(savedBoxCaptureLocals);
        singleCaptureLocals_ = std::move(savedSingleCaptureLocals);
        boxCaptureLocalsFn_ = savedBoxCaptureLocalsFn;
        definitelyEscapingLambdas_ = std::move(savedDefinitelyEscaping);
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
                // fix #1255 / §27.5 / §14.2: when the expression body lowered
                // to null (e.g. `x.Val()` where x: T with T erased to i32 in
                // the unspecialized generic template — the non-pointer receiver
                // guard in emitMethodCall returns null) AND the declared return
                // type is NON-VOID, emitting `ret void` produces malformed IR
                // that the LLVM verifier rejects ("Function return type does
                // not match operand type of return inst! ret void / i32").
                // Mirror emitImplicitReturn and the block-body fall-through
                // below: emit a zero-value stub for non-void return types.
                // The unspecialized template stub is never called in practice
                // (only the per-T specialization built by
                // buildSpecializedMethodFn is called, and that re-lowers the
                // lambda with the concrete T so lowerExpr(x.Val()) returns a
                // real value); for a real codegen error the diagnostic system
                // already reported it and any value is equally "wrong".
                // per §27.5 / §14.2.
                if (retTy->isVoidTy())
                    builder_.CreateRetVoid();
                else
                    builder_.CreateRet(llvm::Constant::getNullValue(retTy));
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
    // #1273 (§9.5 / §7.5): a `with` on a NULLABLE record source (`p: P?`). `src`
    // is the tagged `{i8, P}` nullable struct value, but the record-copy below
    // needs the BARE record `P`. Unwrap the payload (field 1) for the copy and
    // remember the tag; re-wrap the result with that tag at the return (so a null
    // source propagates to a null result). Without this the copy treated the
    // `{i8, P}` value as a bare `P` — mismatched GEP/store layout — and a later
    // `q!.A` hit nullablePrimHasValue on a wrongly-shaped value (LLVM ICmp
    // type-mismatch → SIGABRT).
    const bool srcNullableRecord =
        isRecord && rt && rt->nullable &&
        rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
        rt->declaration->kind == ast::TypeDeclKind::Record &&
        !src->getType()->isPointerTy();
    llvm::Value* nullableTag = nullptr;
    if (srcNullableRecord) {
        nullableTag = builder_.CreateExtractValue(src, {0u}, "with.nul.tag");
        src = builder_.CreateExtractValue(src, {1u}, "with.nul.payload");
    }
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
                if (!v) {
                    // #1386: ICE — lowerExpr returned null for the value in
                    // a with-expression custom-setter assignment.
                    ctx_.diagnostics().report(
                        Severity::Error, a.value->span.begin,
                        "internal: lowerExpr returned null for with-expression "
                        "custom-setter value — #1386");
                    continue;
                }
                if (!setter) continue;
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
        // fix #1382 / §15.4 / §9.5: a `with`-replacement value that is a
        // collection literal `[e1, …]` targeting an INLINE-ARRAY field.
        // lowerExpr returns null for CollectionLiteralExpr, so the plain
        // `lowerExpr` + `if (!v) continue` below silently drops the write,
        // leaving the field holding the source record's original value.
        // Mirror the positional-record constructor path: store elements
        // directly into the field slot via GEP (no aggregate load needed).
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                a.value.get())) {
            if (!cl->isDict) {
                llvm::Value* fslot =
                    builder_.CreateStructGEP(st, base, idxIt->second);
                llvm::Type* fty = st->getElementType(idxIt->second);
                if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(fty)) {
                    llvm::Type* elTy = aty->getElementType();
                    for (std::size_t k = 0;
                         k < cl->elements.size() &&
                         k < aty->getNumElements(); ++k) {
                        if (!cl->elements[k]) continue;
                        llvm::Value* ev = lowerExpr(*cl->elements[k]);
                        if (!ev) {
                            // #1386: ICE — lowerExpr returned null for a
                            // with-expression inline-array field element.
                            ctx_.diagnostics().report(
                                Severity::Error,
                                cl->elements[k]->span.begin,
                                "internal: lowerExpr returned null for "
                                "with-expression inline-array field element"
                                " — #1386");
                            continue;
                        }
                        if (ev->getType() != elTy &&
                            ev->getType()->isIntegerTy() &&
                            elTy->isIntegerTy()) {
                            unsigned eb = ev->getType()->getIntegerBitWidth();
                            unsigned db = elTy->getIntegerBitWidth();
                            ev = db > eb ? builder_.CreateSExt(ev, elTy)
                                         : builder_.CreateTrunc(ev, elTy);
                        }
                        builder_.CreateStore(
                            coerceToNullableSlot(elTy, ev),
                            builder_.CreateGEP(
                                aty, fslot,
                                {builder_.getInt32(0),
                                 builder_.getInt32(
                                     static_cast<unsigned>(k))}));
                    }
                    continue;  // field handled — skip the lowerExpr path below
                }
            }
        }
        llvm::Value* v = lowerExpr(*a.value);
        if (!v) {
            // #1386: ICE — lowerExpr returned null for a with-expression field
            // replacement value; the expression kind is not yet handled.
            ctx_.diagnostics().report(
                Severity::Error, a.value->span.begin,
                "internal: lowerExpr returned null for with-expression "
                "field/property replacement value — #1386");
            continue;
        }
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
        // store the tagged {i8,T} struct, not the raw scalar/payload — otherwise
        // the bare value overwrites the hasValue byte and leaves the value half
        // stale (wrong value, or coincidentally non-null). Wrap the replacement
        // value exactly as the positional-constructor path does.
        // fix #1130 / §9.7: the value-half T may itself be a STRUCT — a nullable
        // RECORD field (`Inner?`) whose override is a non-null record value
        // `new Inner(5)` of type `%record.Inner`. The old guard `!isStructTy()`
        // excluded exactly that case, so the non-null→nullable widening was
        // skipped: the bare `%record.Inner` was stored over the `{i8,%record.Inner}`
        // slot, clobbering the tag and leaving the payload half = the source
        // record's bytes (rc=3 in the repro; zeroed payload when source was null).
        // Wrap whenever `v` is not already the tagged struct, matching the value
        // half by exact type or layout-identity (the specialized-vs-unspecialized
        // record-struct mismatch a monomorphized body induces, #460). (Narrow
        // nullable fields like `byte?`, where the lowered value width differs from
        // the element width, are a separate gap tracked in the R40 follow-up.)
        if (const sema::TypeRef* fty = recordMemberTypeResolved(m)) {
            if (isNullableValueTy(fty)) {
                auto* expectedSt = nullableValueStructTy(fty);
                llvm::Type* slotTy = expectedSt->getElementType(1);
                if (v->getType() == expectedSt) {
                    // Already the tagged {i8,T} struct (override value already
                    // typed T?, variant A) — store as-is.
                } else if (v->getType()->isPointerTy() &&
                           llvm::isa<llvm::ConstantPointerNull>(v)) {
                    v = nullableValueNull(fty);
                } else if (v->getType() == slotTy) {
                    v = wrapNullableValue(fty, v);
                } else if (auto* vSt =
                               llvm::dyn_cast<llvm::StructType>(v->getType())) {
                    if (auto* sSt = llvm::dyn_cast<llvm::StructType>(slotTy))
                        if (vSt->isLayoutIdentical(sSt))
                            v = wrapNullableValue(fty, v);
                }
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
    llvm::Value* result = isRecord ? builder_.CreateLoad(st, base) : base;
    // #1273: re-wrap the copied record into the tagged `{i8, P}` nullable struct,
    // carrying the source's tag so a null source stays null.
    if (srcNullableRecord && nullableTag) {
        llvm::StructType* nst =
            llvm::StructType::get(llvm_, {nullablePrimHasValTy(), st});
        llvm::Value* wrapped = llvm::UndefValue::get(nst);
        wrapped = builder_.CreateInsertValue(wrapped, nullableTag, {0u});
        wrapped = builder_.CreateInsertValue(wrapped, result, {1u});
        return wrapped;
    }
    return result;
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


llvm::Value* Emitter::emitExcCauseSnapshot(llvm::Value* cause,
                                           bool causeIsBuiltin) {
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    // Virtual ToString (vtable slot 3): vtable = cause[0]; ToString = vtable[3].
    // For a built-in exception this is the §8.5 default — its type name.
    llvm::Value* vtable = builder_.CreateLoad(ptrTy(), cause, "cause.vt");
    llvm::Value* tsSlot = builder_.CreateGEP(
        ptrTy(), vtable, builder_.getInt64(3), "cause.ts.slot");
    llvm::Value* tsFn = builder_.CreateLoad(ptrTy(), tsSlot, "cause.ts.fn");
    auto* tsFnTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    llvm::Value* typeName =
        builder_.CreateCall(tsFnTy, tsFn, {cause}, "cause.tn");
    // Message @ kExcMsgOffset.
    llvm::Value* msg = builder_.CreateLoad(ptrTy(),
        builder_.CreateGEP(i8Ty, cause, builder_.getInt64(kExcMsgOffset)),
        "cause.msg");
    // Nested chain @ kExcCauseOffset — only a built-in cause has this slot.
    llvm::Value* nested =
        causeIsBuiltin
            ? builder_.CreateLoad(ptrTy(),
                  builder_.CreateGEP(i8Ty, cause,
                      builder_.getInt64(kExcCauseOffset)),
                  "cause.nested")
            : static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(ptrTy()));
    auto* fn = declareC("bz_exc_causechain_link", ptrTy(),
                        {ptrTy(), ptrTy(), ptrTy()});
    return builder_.CreateCall(fn, {typeName, msg, nested}, "cc.link");
}


llvm::Value* Emitter::lowerExceptionInfoField(const ast::Expr& target,
                                              unsigned fieldIdx) {
    // ExceptionInfo lowers to a 24-byte {TypeName, Message, StackTrace} record
    // of three bz_string* (LlvmBackend_Layout.cpp).
    auto* eiTy = llvm::StructType::get(llvm_, {ptrTy(), ptrTy(), ptrTy()});
    // Common case: `<array>[i]` where <array> is Array<ExceptionInfo>. Copy the
    // element out with bz_array_get (bounds-checked) and read the field.
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&target)) {
        if (ix->target && !ix->indices.empty() && ix->indices[0].value) {
            llvm::Value* arr = lowerExpr(*ix->target);
            llvm::Value* idx = lowerExpr(*ix->indices[0].value);
            if (!arr || !idx) return nullptr;
            llvm::Value* idx64 = idx->getType() == i64()
                ? idx
                : builder_.CreateSExt(idx, i64(), "ei.idx64");
            // §15.1: bounds-check (unsigned, so a negative index is rejected too).
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {arr}, "ei.len");
            emitGuardThrow(builder_.CreateICmpUGE(idx64, len, "ei.oor"),
                           "IndexOutOfRangeException");
            llvm::AllocaInst* tmp = entryAlloca(eiTy);
            auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(getFn,
                {arr, idx64, tmp, builder_.getInt64(24)});
            llvm::Value* fp =
                builder_.CreateStructGEP(eiTy, tmp, fieldIdx, "ei.fld");
            return builder_.CreateLoad(ptrTy(), fp, "ei.val");
        }
    }
    // Fallback: an addressable ExceptionInfo value — lower it and re-store to a
    // temp so the field can be GEP'd.
    if (llvm::Value* v = lowerExpr(target)) {
        llvm::AllocaInst* tmp = entryAlloca(eiTy);
        builder_.CreateStore(v, tmp);
        llvm::Value* fp =
            builder_.CreateStructGEP(eiTy, tmp, fieldIdx, "ei.fld");
        return builder_.CreateLoad(ptrTy(), fp, "ei.val");
    }
    return nullptr;
}


llvm::Value* Emitter::lowerNew(const ast::NewExpr& ne) {
    // §17.7 `new(args)` shorthand: there is no explicit `ne.type` — the target
    // type is inferred from context (the LHS annotation / assignment target) and
    // recorded by sema as the NewExpr's own exprType. Derive the construction
    // target from that exprType. Without this the construction was skipped and
    // the local was never initialized; a later read of the uninitialized slot
    // dereferenced garbage → SIGSEGV (issue #979).
    sema::TypeRef* rt = ne.type
        ? ctx_.resolvedType(*ne.type)
        : const_cast<sema::TypeRef*>(ctx_.exprType(ne));
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return nullptr;
    // The ast::Type name node used for the builtin-exception / collection
    // dispatch below: the explicit type node when present, else the inferred
    // target's node (a shorthand targeting a builtin collection, e.g.
    // `var x: Array<int> := new()`).
    const ast::Type* tnode = ne.type ? ne.type.get() : rt->astNode;
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
        auto sit = genericSubstSemaTy_.find(rt->headName());
        if (sit != genericSubstSemaTy_.end() && sit->second &&
            sit->second->kind == sema::TypeRef::Kind::Named &&
            sit->second->declaration) {
            rt = const_cast<sema::TypeRef*>(sit->second);
        }
    }
    // §22.4 (#968): pre-create the `out var x` slot(s) for `new T(out var x)` so
    // the arg lowering below passes the slot's address and a later read sees the
    // value the constructor wrote (the call-site analogue of ensureOutVarSlots).
    ensureCtorOutVarSlots(ne, rt);
    if (!rt->declaration) {
        // CG22: built-in exception types (Exception, ArgumentException, …)
        // have no user TypeSymbol (rt->declaration == null) but do have a
        // built-in vtable from setupBuiltinExceptions. Construct a 16-byte
        // object — [vtable_ptr, message_ptr] — so that e.Message works.
        if (tnode && tnode->name && !tnode->name->parts.empty()) {
            std::string n(tnode->name->parts.back());
            auto vit = builtinVtable_.find(n);
            if (vit != builtinVtable_.end()) {
                // §31.4: kExcObjSize block — vtable@0, Message@8, CauseChain@24,
                // ParamName@32 (calloc zeroes the unused slots).
                llvm::Value* obj = builder_.CreateCall(
                    callocFn(),
                    {builder_.getInt64(1), builder_.getInt64(kExcObjSize)});
                builder_.CreateStore(vit->second, obj);
                auto* i8Ty2 = llvm::Type::getInt8Ty(llvm_);
                // arg0 = message (string).
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* msgStr = lowerExpr(*ne.args[0].value);
                    if (msgStr) {
                        llvm::Value* msgSlot = builder_.CreateGEP(
                            i8Ty2, obj, builder_.getInt64(kExcMsgOffset));
                        builder_.CreateStore(msgStr, msgSlot);
                    }
                }
                // arg1 distinguishes the two-argument constructors by static type
                // (§31.4): a `string` second argument is ArgumentException's
                // paramName (→ ParamName@32); anything else is the (message,
                // cause) overload's cause, snapshotted into CauseChain@24 (#811).
                if (ne.args.size() >= 2 && ne.args[1].value) {
                    const sema::TypeRef* a1ty = ctx_.exprType(*ne.args[1].value);
                    bool argIsString = a1ty &&
                        a1ty->kind == sema::TypeRef::Kind::Primitive &&
                        a1ty->primitive == sema::PrimitiveType::String;
                    if (argIsString) {
                        llvm::Value* pn = lowerExpr(*ne.args[1].value);
                        if (pn) {
                            llvm::Value* pnSlot = builder_.CreateGEP(
                                i8Ty2, obj, builder_.getInt64(kExcParamOffset));
                            builder_.CreateStore(pn, pnSlot);
                        }
                    } else {
                        // (message, cause): snapshot the borrowed cause (#811).
                        llvm::Value* cause = lowerExpr(*ne.args[1].value);
                        if (cause) {
                            // §31.4: a USER exception subclass also carries
                            // the full built-in header (classStruct), so its
                            // nested CauseChain@24 is readable too.
                            bool hasHdr = isBuiltinExceptionType(a1ty) ||
                                (a1ty && a1ty->declaration &&
                                 hasBuiltinExceptionBase(a1ty->declaration));
                            llvm::Value* chain = emitExcCauseSnapshot(
                                cause, /*causeIsBuiltin=*/hasHdr);
                            llvm::Value* ccSlot = builder_.CreateGEP(
                                i8Ty2, obj, builder_.getInt64(kExcCauseOffset));
                            builder_.CreateStore(chain, ccSlot);
                            // §31.1: the cause is BORROWED only for the snapshot.
                            // A fresh `new` temp in argument position is owned by
                            // no one afterwards — dispose+reclaim it now (its data
                            // is already copied). A named variable / field cause
                            // stays with its own owner (not a NewExpr → skipped).
                            if (dynamic_cast<const ast::NewExpr*>(
                                    ne.args[1].value.get()))
                                emitDisposeAndReclaim(cause);
                        }
                    }
                }
                return obj;
            }
        }
        // §3: stdlib collection types — allocate via runtime.
        if (tnode && tnode->name && !tnode->name->parts.empty()) {
            std::string_view colName = tnode->name->parts[0];
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
            // §4.14 (#1275): direct span constructors —
            //   new Span<T>(buffer, start, length)
            //   new ReadOnlySpan<T>(buffer, start, length)
            //   new ArraySpan<T>(source, start, length)
            //   new ReadOnlyArraySpan<T>(source, start, length)
            // These records lower to the { ptr source, i32 start, i32 length }
            // arrayspan struct (isSpanLikeTypeName). Constructing them via the
            // generic record path built a value-layout struct with i64
            // Start/Length, so the later i32 span reads picked up garbage and
            // indexed out of bounds (SIGSEGV). Lower the buffer/array arg to its
            // bz_array_t* backing and build the span directly, bounds-checking
            // start/length against the backing Count (IndexOutOfRangeException,
            // per §4.14 L1940 / L2179 — the documented supported constructor).
            if ((colName == "Span" || colName == "ReadOnlySpan" ||
                 colName == "ArraySpan" || colName == "ReadOnlyArraySpan") &&
                ne.args.size() == 3 &&
                ne.args[0].value && ne.args[1].value && ne.args[2].value) {
                llvm::Value* src = lowerExpr(*ne.args[0].value);  // → bz_array_t*
                if (!src) return nullptr;
                llvm::Value* start = lowerExpr(*ne.args[1].value);
                llvm::Value* len   = lowerExpr(*ne.args[2].value);
                if (!start || !len) return nullptr;
                if (!start->getType()->isIntegerTy(32))
                    start = builder_.CreateTrunc(start, i32(), "span.ctor.st");
                if (!len->getType()->isIntegerTy(32))
                    len = builder_.CreateTrunc(len, i32(), "span.ctor.len");
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* count64 =
                    builder_.CreateCall(lenFn, {src}, "span.ctor.cnt");
                llvm::Value* count =
                    builder_.CreateTrunc(count64, i32(), "span.ctor.cnt32");
                // Start ∈ [0, Count]; Length ∈ [0, Count-Start].
                llvm::Value* startNeg =
                    builder_.CreateICmpSLT(start, builder_.getInt32(0));
                llvm::Value* startOOB = builder_.CreateICmpSGT(start, count);
                emitGuardThrow(builder_.CreateOr(startNeg, startOOB),
                               "IndexOutOfRangeException");
                llvm::Value* avail =
                    builder_.CreateSub(count, start, "span.ctor.avail");
                llvm::Value* lenNeg =
                    builder_.CreateICmpSLT(len, builder_.getInt32(0));
                llvm::Value* lenOOB = builder_.CreateICmpSGT(len, avail);
                emitGuardThrow(builder_.CreateOr(lenNeg, lenOOB),
                               "IndexOutOfRangeException");
                return buildArraySpan(src, start, len);
            }
            // issues #343 / D8 / §4.10: a Set<T> (or OwningSet<T>) is backed by
            // the generic-key hash table so equality and hashing follow T's
            // contract (§8.3) — real set semantics (dedup by content). This
            // covers records (#343), primitives/enums, AND strings (D8: the
            // array backing cannot deduplicate). Stored value is a non-null
            // sentinel (membership only).
            if ((colName == "Set" || colName == "OwningSet" ||
                 // stdlib §4.18 (#1426): SortedSet<T> shares the generic-key
                 // hash table backing of Set<T> (bz_dict_alloc_generic with a
                 // sentinel value for membership-only entries).  Without this
                 // branch lowerNew returned nullptr, leaving the slot uninitialized
                 // — same root cause as #1406 for SortedDictionary.
                 colName == "SortedSet")) {
                // stdlib §1/§4.4 (v0.183, #1396): `Set<T>(capacity: int)` /
                // `OwningSet<T>(capacity: int)` fault on a negative capacity
                // rather than silently ignoring it. The first arg is the
                // capacity form only when it is a non-nullable int (the sibling
                // `constructor(comparer: IEqualityComparer<T>)` overload's arg
                // is not, and is left untouched here).
                if (!ne.args.empty() && ne.args[0].value) {
                    const sema::TypeRef* a0ty = ctx_.exprType(*ne.args[0].value);
                    if (a0ty && a0ty->kind == sema::TypeRef::Kind::Primitive &&
                        a0ty->primitive == sema::PrimitiveType::Int &&
                        !a0ty->nullable) {
                        llvm::Value* capArg = lowerExpr(*ne.args[0].value);
                        if (capArg) {
                            if (!capArg->getType()->isIntegerTy(64))
                                capArg = builder_.CreateSExt(capArg, i64());
                            emitGuardThrow(
                                builder_.CreateICmpSLT(capArg,
                                                       builder_.getInt64(0)),
                                "ArgumentOutOfRangeException");
                        }
                    }
                }
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
            // per §4.6/§4.7: LinkedList/OwningLinkedList use the real doubly-linked-
            // list runtime (bz_llist_alloc), not the bz_array_t backing. The
            // elem_size argument tells the runtime how many bytes to store inline
            // in each node — same sizing as bz_array_t (4 for int/float, 8 otherwise).
            if (colName == "LinkedList" || colName == "OwningLinkedList") {
                auto* allocFn = declareC("bz_llist_alloc", ptrTy(), {i64()});
                return builder_.CreateCall(allocFn, {builder_.getInt64(elemSize)});
            }
            if (colName == "Array" || colName == "OwningArray" ||
                // §4.14: OwningBuffer<T> uses the same bz_array_t* backing.
                // new OwningBuffer<T>(capacity) pre-allocates `capacity` slots
                // (all zero-initialized), mirroring the Array no-arg ctor.
                colName == "OwningBuffer" ||
                colName == "Stack" || colName == "OwningStack" ||
                colName == "Queue" || colName == "OwningQueue" ||
                colName == "Set" || colName == "OwningSet") {
                // For OwningBuffer<T>(n): the arg is the capacity / initial count.
                // Pre-allocate n slots AND set count=n so index-assignment is valid.
                if (colName == "OwningBuffer" && !ne.args.empty() && ne.args[0].value) {
                    llvm::Value* nArg = lowerExpr(*ne.args[0].value);
                    if (!nArg) return nullptr;
                    if (!nArg->getType()->isIntegerTy(64))
                        nArg = builder_.CreateSExt(nArg, i64());
                    // stdlib §1/§4.4 (v0.183, #1396): OwningBuffer<T>(length) and
                    // its (length, init) overload both fault on negative length
                    // rather than silently allocating an empty buffer.
                    emitGuardThrow(
                        builder_.CreateICmpSLT(nArg, builder_.getInt64(0)),
                        "ArgumentOutOfRangeException");
                    // bz_array_alloc_count_n: alloc with capacity n and set count=n.
                    // (All slots are zero-initialized by calloc inside the runtime.)
                    auto* fn = declareC("bz_array_alloc_count_n", ptrTy(),
                                       {i64(), i64()});
                    llvm::Value* buf = builder_.CreateCall(fn,
                        {builder_.getInt64(elemSize), nArg}, "ob.arr");
                    // §4.14 (#778): the two-argument constructor
                    // `OwningBuffer<T>(length, init: (int) -> T)` calls `init(i)`
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
                            // §4.14 (#1286): the init lambda is call-scoped — it
                            // fills the buffer and does not escape. Release the
                            // producer's +1 so its closure cell (and env) are
                            // freed; without this the closure leaked (LSan: 32
                            // bytes for a non-capturing `(i) => …`). initClosure
                            // is the { fn, env, envdtor, rc } cell pointer (the
                            // loop loads fn/env from its slots). bz_closure_release
                            // is null-safe and, with today's null envdtor, frees
                            // only the cell + env — no captured-slot
                            // double-release — and the closure is released nowhere
                            // else, so this is an exactly-once drop.
                            auto* relFn = declareC(
                                "bz_closure_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(relFn, {initClosure});
                        }
                    }
                    return buf;
                }
                // per stdlib §4.4 v0.165 (#1375): Array<T>(size: int, fill: T)
                // — allocate `size` slots (count set to size) and fill each one
                // with the `fill` value. Models the two-argument Array constructor
                // that initialises every element to a known value (e.g.
                // `new Array<byte>(16, 0b)` or `new Array<int>(4, 42)`).
                // Value types (byte, int, …) are stored by copy; class/interface
                // elements are stored as borrowed references (Array<T> is always
                // a borrowing collection — §4.4 — so aliasing is fine).
                if (colName == "Array" && ne.args.size() == 2 &&
                    ne.args[0].value && ne.args[1].value) {
                    llvm::Value* sizeArg = lowerExpr(*ne.args[0].value);
                    if (!sizeArg) return nullptr;
                    if (!sizeArg->getType()->isIntegerTy(64))
                        sizeArg = builder_.CreateSExt(sizeArg, i64());
                    // stdlib §1/§4.4 (v0.183, #1396): Array<T>(size, fill) faults
                    // on a negative size rather than silently building an
                    // empty array.
                    emitGuardThrow(
                        builder_.CreateICmpSLT(sizeArg, builder_.getInt64(0)),
                        "ArgumentOutOfRangeException");
                    // bz_array_alloc_count_n: alloc with capacity=n, count=n,
                    // all slots zero-initialised (calloc inside the runtime).
                    auto* fn = declareC("bz_array_alloc_count_n", ptrTy(),
                                       {i64(), i64()});
                    llvm::Value* buf = builder_.CreateCall(
                        fn, {builder_.getInt64(elemSize), sizeArg}, "arr.fill.buf");
                    // Compute the LLVM element type for the fill value.
                    llvm::Type* elemLlvmTy = i32();
                    if (!rt->genericArgs.empty() && rt->genericArgs[0])
                        if (auto* et = lowerTypeRef(rt->genericArgs[0]))
                            elemLlvmTy = et;
                    llvm::Value* fillVal = lowerExpr(*ne.args[1].value);
                    if (fillVal) {
                        // Spill fill value into an alloca so bz_array_set
                        // can read it via a pointer (matching OwningBuffer init
                        // loop pattern at §4.14 / #778).
                        llvm::AllocaInst* fillSlot = entryAlloca(elemLlvmTy);
                        builder_.CreateStore(fillVal, fillSlot);
                        // for i = 0 ..<  sizeArg: bz_array_set(buf, i, fillSlot, esz)
                        llvm::AllocaInst* iSlot = entryAlloca(i64());
                        builder_.CreateStore(builder_.getInt64(0), iSlot);
                        auto* hdrBB  = llvm::BasicBlock::Create(
                            llvm_, "arr.fill.hdr",  currentFn_);
                        auto* bodyBB = llvm::BasicBlock::Create(
                            llvm_, "arr.fill.body", currentFn_);
                        auto* doneBB = llvm::BasicBlock::Create(
                            llvm_, "arr.fill.done", currentFn_);
                        builder_.CreateBr(hdrBB);
                        builder_.SetInsertPoint(hdrBB);
                        llvm::Value* iv =
                            builder_.CreateLoad(i64(), iSlot, "arr.fill.i");
                        builder_.CreateCondBr(
                            builder_.CreateICmpSLT(iv, sizeArg), bodyBB, doneBB);
                        builder_.SetInsertPoint(bodyBB);
                        auto* setFn = declareC("bz_array_set",
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), i64(), ptrTy(), i64()});
                        builder_.CreateCall(setFn,
                            {buf, iv, fillSlot, builder_.getInt64(elemSize)});
                        builder_.CreateStore(
                            builder_.CreateAdd(iv, builder_.getInt64(1)), iSlot);
                        builder_.CreateBr(hdrBB);
                        builder_.SetInsertPoint(doneBB);
                    }
                    return buf;
                }
                // stdlib §4.4/§4.5 (#741): the `constructor(capacity: int)` arg
                // reserves backing slots WITHOUT setting count (an empty array
                // with room to grow), distinct from OwningBuffer above which also
                // sets count=n. Previously the arg was ignored and a constant 16
                // was always allocated, so `.Capacity` could not reflect the
                // requested reservation. A non-positive request falls back to 16.
                auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* capArg = lowerExpr(*ne.args[0].value);
                    if (capArg) {
                        if (!capArg->getType()->isIntegerTy(64))
                            capArg = builder_.CreateSExt(capArg, i64());
                        // stdlib §1/§4.4 (v0.183, #1396): Array<T>(capacity) /
                        // OwningArray<T>(capacity) (and the Set<T>/OwningSet<T>
                        // capacity forms that fall through to this shared
                        // backing) fault on a negative capacity — zero remains
                        // valid and is handled by the max(capArg, 1) below.
                        emitGuardThrow(
                            builder_.CreateICmpSLT(capArg, builder_.getInt64(0)),
                            "ArgumentOutOfRangeException");
                        // max(capArg, 1) so a 0 request still yields a
                        // usable buffer; matches the OwningArray reference impl
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
            if (colName == "Dictionary" || colName == "OwningDictionary" ||
                // stdlib §4.17 (#1354, #1406): SortedDictionary<K,V> shares
                // the bz_dict_t* runtime representation (same hash-table backing
                // as Dictionary). Without this branch lowerNew returned nullptr,
                // leaving the variable slot uninitialized — bz_dict_* calls then
                // received a zero/i32 receiver and produced malformed IR.
                colName == "SortedDictionary") {
                // stdlib §1/§4.8 (v0.183, #1396): `Dictionary<K,V>(capacity: int)`
                // / `OwningDictionary<K,V>(capacity: int)` (bare or paired with a
                // comparer) fault on a negative capacity. The first arg is the
                // capacity form only when it is a non-nullable int (the sibling
                // `constructor(comparer: IEqualityComparer<K>)` overload's arg
                // is not, and is left untouched here). Capacity is not otherwise
                // used to size the backing table below — only the fault matters.
                if (!ne.args.empty() && ne.args[0].value) {
                    const sema::TypeRef* a0ty = ctx_.exprType(*ne.args[0].value);
                    if (a0ty && a0ty->kind == sema::TypeRef::Kind::Primitive &&
                        a0ty->primitive == sema::PrimitiveType::Int &&
                        !a0ty->nullable) {
                        llvm::Value* capArg = lowerExpr(*ne.args[0].value);
                        if (capArg) {
                            if (!capArg->getType()->isIntegerTy(64))
                                capArg = builder_.CreateSExt(capArg, i64());
                            emitGuardThrow(
                                builder_.CreateICmpSLT(capArg,
                                                       builder_.getInt64(0)),
                                "ArgumentOutOfRangeException");
                        }
                    }
                }
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
                    } else if (argTy &&
                               argTy->kind == sema::TypeRef::Kind::Primitive &&
                               argTy->primitive == sema::PrimitiveType::Int &&
                               !argTy->nullable) {
                        // stdlib §1/§3.1 (v0.183, #1396): StringBuilder(capacity)
                        // faults on a negative capacity. The capacity value is
                        // not otherwise used to size the backing buffer above
                        // (a fixed 64-slot reservation) — only the fault matters.
                        llvm::Value* capArg = lowerExpr(*ne.args[0].value);
                        if (capArg) {
                            if (!capArg->getType()->isIntegerTy(64))
                                capArg = builder_.CreateSExt(capArg, i64());
                            emitGuardThrow(
                                builder_.CreateICmpSLT(capArg,
                                                       builder_.getInt64(0)),
                                "ArgumentOutOfRangeException");
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
                // v0.174 (#809) / §6.9: bounded ctor `new Channel<T>(capacity)`.
                // capacity >= 1, else ArgumentException (a catchable guard, per
                // §6.9). The nullary ctor passes capacity 0 (= unbounded).
                llvm::Value* capArg = llvm::ConstantInt::get(i64(), 0);
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* cap = lowerExpr(*ne.args[0].value);
                    if (cap) {
                        if (!cap->getType()->isIntegerTy(32))
                            cap = builder_.CreateIntCast(cap, i32(),
                                                         /*isSigned=*/true, "cap.i32");
                        // capacity < 1 → ArgumentException (thrown before we
                        // allocate, so no channel leaks on the guard path).
                        emitGuardThrow(
                            builder_.CreateICmpSLT(cap, builder_.getInt32(1),
                                                   "cap.bad"),
                            "ArgumentException");
                        capArg = builder_.CreateSExt(cap, i64(), "cap.i64");
                    }
                }
                auto* allocFn = declareC("bz_channel_new", ptrTy(),
                                         {ptrTy(), i64()});
                return builder_.CreateCall(allocFn, {dispArg, capArg}, "ch.new");
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
            // §6.6 / issue #974: new TaskCompletionSource<T>() /
            // new OwningTaskCompletionSource<T>() / new TaskCompletionSource().
            // Allocate via bz_tcs_new() — zero arguments (constructor takes none).
            // §28.3 / #1250: for OwningTaskCompletionSource<T:class>, wire the element
            // disposer via bz_tcs_set_owned_disposer so TCS disposal can run the
            // destructor of a set-but-unconsumed owned result (exactly-once, §28.3).
            if (colName == "TaskCompletionSource" ||
                colName == "OwningTaskCompletionSource") {
                auto* allocFn = declareC("bz_tcs_new", ptrTy(), {});
                llvm::Value* tcs = builder_.CreateCall(allocFn, {}, "tcs.new");
                if (colName == "OwningTaskCompletionSource" && rt &&
                    !rt->genericArgs.empty() && rt->genericArgs[0]) {
                    llvm::Function* dispThunk =
                        elementPolicyFor(rt->genericArgs[0],
                                         /*owningFamily=*/true).disposerThunk;
                    if (dispThunk) {
                        auto* setFn = declareC("bz_tcs_set_owned_disposer",
                                               llvm::Type::getVoidTy(llvm_),
                                               {ptrTy(), ptrTy()});
                        builder_.CreateCall(setFn, {tcs, dispThunk});
                    }
                }
                return tcs;
            }
            // §6.4 / issue #647: new Atomic<T>(initial) / new AtomicInteger<T>(initial).
            // Allocate via bz_atomic_new(cell_size, initial).
            // cell_size is derived from T: 1 (bool/byte/sbyte/char), 2 (short/ushort),
            //   4 (int/uint — default), 8 (long/ulong/IntPtr/UIntPtr).
            // initial comes from the constructor argument (zero if not provided).
            if (colName == "Atomic" || colName == "AtomicInteger") {
                // Determine cell_size from the type argument T.
                int cellSize = 4;  // default: int/uint
                const sema::TypeRef* newTR = rt;
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
                const sema::TypeRef* newTR = rt;
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
            // stdlib §6.10 (v0.175, #808): new TaskScope() — allocate via
            // bz_task_scope_new (fresh internal cancellation cell).
            if (colName == "TaskScope") {
                auto* allocFn = declareC("bz_task_scope_new", ptrTy(), {});
                return builder_.CreateCall(allocFn, {}, "tscope.alloc");
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
            // per §2.5 / #1357: new DateOnly(year, month, day) — stored as i32 day number.
            // Compute: bz_datetime_parts_to_ticks(y,m,d,0,0,0,0) / TICKS_PER_DAY
            if (colName == "DateOnly") {
                auto getI32do = [&](size_t idx) -> llvm::Value* {
                    if (idx >= ne.args.size() || !ne.args[idx].value)
                        return builder_.getInt32(1);
                    llvm::Value* v = lowerExpr(*ne.args[idx].value);
                    if (!v) return builder_.getInt32(1);
                    if (!v->getType()->isIntegerTy(32))
                        v = builder_.CreateTrunc(v, i32());
                    return v;
                };
                llvm::Value* yr  = getI32do(0);
                llvm::Value* mo  = getI32do(1);
                llvm::Value* day = getI32do(2);
                auto* fn = declareC("bz_datetime_parts_to_ticks", i64(),
                    {i32(), i32(), i32(), i32(), i32(), i32(), i32()});
                llvm::Value* ticks = builder_.CreateCall(fn,
                    {yr, mo, day, builder_.getInt32(0), builder_.getInt32(0),
                     builder_.getInt32(0), builder_.getInt32(0)}, "do.ctor.ticks");
                llvm::Value* tpd = builder_.getInt64(864000000000LL);
                return builder_.CreateTrunc(
                    builder_.CreateSDiv(ticks, tpd, "do.ctor.dn"),
                    i32(), "do.ctor");
            }
            // per §2.5 / #1357: new TimeOnly(hour, minute, second[, ms]) — stored
            // as i32 ms from midnight.
            if (colName == "TimeOnly") {
                auto getI32to = [&](size_t idx) -> llvm::Value* {
                    if (idx >= ne.args.size() || !ne.args[idx].value)
                        return builder_.getInt32(0);
                    llvm::Value* v = lowerExpr(*ne.args[idx].value);
                    if (!v) return builder_.getInt32(0);
                    if (!v->getType()->isIntegerTy(32))
                        v = builder_.CreateTrunc(v, i32());
                    return v;
                };
                llvm::Value* hr  = getI32to(0);
                llvm::Value* mn  = getI32to(1);
                llvm::Value* sc  = getI32to(2);
                llvm::Value* ms  = getI32to(3);
                // ms_from_midnight = hr*3600000 + mn*60000 + sc*1000 + ms
                llvm::Value* r = builder_.CreateMul(hr, builder_.getInt32(3600000));
                r = builder_.CreateAdd(r, builder_.CreateMul(mn, builder_.getInt32(60000)));
                r = builder_.CreateAdd(r, builder_.CreateMul(sc, builder_.getInt32(1000)));
                r = builder_.CreateAdd(r, ms);
                return r;
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
                    genericSubst_.count(arg->headName())) {
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
                        // #1605 / §4.6 / §28.9: `fs` is a genuine class
                        // FieldSymbol (disposed by the generated DisposeCore /
                        // emitOwnedFieldDisposals like any other string field,
                        // whether or not the parameter is also read from a body
                        // position) — so a STRING positional-ctor-param field
                        // needs its own independent +1 here, exactly like
                        // lowerStringFieldStore / emitOwnFieldInit give any other
                        // string field store. Without this, the field ALIASES
                        // the caller's argument with no reference of its own: if
                        // the caller later releases its local (e.g. `x += ...`'s
                        // release-old, or scope-end), the field dangles and
                        // Dispose() reads/frees already-freed memory (#1605). The
                        // field is calloc-zeroed (null) before this — no old
                        // value to release. A fresh +1 producer argument
                        // (`new Box(a + b)`) is TRANSFERRED (no retain); anything
                        // else (identifier / field / call) is RETAINED so the
                        // field's count is independent of the source's.
                        if (v->getType()->isPointerTy() &&
                            isStringStorageMember(fs) &&
                            !isPlusOneStringProducer(ne.args[pi].value.get()))
                            emitStringRetain(v);
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
        // §22.8 step 2 (#929/#930): after the base sequence (step 1), run this
        // no-explicit-ctor class's OWN field initializers — emitInstanceInit at
        // the `new` site now installs only the vtable, and there is no ctor body
        // to run them. A base level's own fields were already initialized inside
        // emitImplicitBase (its base ctor, or its no-ctor field-init).
        // currentThis_ must be `obj` here so a field initializer that reads a
        // primary-ctor parameter (a captured positional field) resolves via
        // implicitFieldAddr against the NEW object, not the enclosing method's
        // `this` (which was restored after the vtable install above).
        llvm::Value* savedThisNoCtor = currentThis_;
        currentThis_ = obj;
        emitImplicitBase(t, obj);
        // §22.7 step 2 (#1291): a throwing OWN field initializer must dispose the
        // earlier owned fields (whose initializers completed, in reverse
        // declaration order) and the base subobject, then reclaim the instance
        // block — mirroring inlineCtorBody's cleanup for the explicit-ctor case.
        // Without this the no-explicit-ctor path leaked the earlier owned fields:
        // emitNewPendingCheck below reclaims only the instance BLOCK, not its
        // owned field objects. The cleanup routes a throwing field init through
        // owned-field + base disposal (emitOwnedFieldDisposals is null-safe, so
        // the not-yet-initialized field of the throwing initializer is a no-op)
        // and then reclaims the block. Set only around the field-init sequence,
        // so a base-ctor throw (emitImplicitBase, above) is still handled by
        // emitNewPendingCheck — the base's own cleanup already disposed the base.
        {
            llvm::BasicBlock* savedUnwind = unwindTarget_;
            std::size_t savedDepth = unwindScopeDepth_;
            auto* savedInsert = builder_.GetInsertBlock();
            llvm::BasicBlock* enclosingUnwind = unwindBB();
            auto* cleanupBB = llvm::BasicBlock::Create(
                llvm_, "noctor.field.cleanup", currentFn_);
            builder_.SetInsertPoint(cleanupBB);
            emitOwnedFieldDisposals(t);
            emitBaseDisposeChain(t);
            builder_.CreateCall(freeFn(), {obj});
            builder_.CreateBr(enclosingUnwind);
            builder_.SetInsertPoint(savedInsert);
            unwindTarget_ = cleanupBB;
            unwindScopeDepth_ = disposeStack_.size();
            emitOwnFieldInit(t, obj, st);
            unwindTarget_ = savedUnwind;
            unwindScopeDepth_ = savedDepth;
        }
        currentThis_ = savedThisNoCtor;
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
// §6.4 (discovery sweep #6): decimal arithmetic for a compound assignment or
// increment (`d += x`, `d /= x`, `d++`). `lhs`/`rhs` are bare %bz.decimal struct
// VALUES. Mirrors the decimal branch of the binary-operator lowering: a div/mod
// guard against a zero divisor, then call bz_decimal_<op> with the
// OverflowException vtable so the runtime can raise on overflow (emitPendingCheck
// unwinds). Returns the result decimal value, or null for a non-arithmetic op.
// Needed because `decimal` is a Kind::Primitive struct, so the compound-assign /
// increment user-operator guards (which require Kind::Named) miss it and the
// integer emitCompoundOp path would feed the struct to an integer overflow
// intrinsic (IR_FAIL / a `/=` SIGABRT).
llvm::Value* Emitter::emitDecimalArith(const std::string& op,
                                       llvm::Value* lhs, llvm::Value* rhs) {
    if (!(op == "+" || op == "-" || op == "*" || op == "/" || op == "mod"))
        return nullptr;
    llvm::StructType* dst = decimalStructType();
    if (op == "/" || op == "mod") {
        llvm::Value* lo  = builder_.CreateExtractValue(rhs, {0u});
        llvm::Value* mid = builder_.CreateExtractValue(rhs, {1u});
        llvm::Value* hi  = builder_.CreateExtractValue(rhs, {2u});
        llvm::Value* orv = builder_.CreateOr(builder_.CreateOr(lo, mid), hi);
        emitGuardThrow(
            builder_.CreateICmpEQ(orv, llvm::ConstantInt::get(i32(), 0)),
            "DivideByZeroException");
    }
    llvm::AllocaInst* aSlot = entryAlloca(dst);
    llvm::AllocaInst* bSlot = entryAlloca(dst);
    llvm::AllocaInst* oSlot = entryAlloca(dst);
    builder_.CreateStore(lhs, aSlot);
    builder_.CreateStore(rhs, bSlot);
    const char* fn_name =
        op == "+" ? "bz_decimal_add" :
        op == "-" ? "bz_decimal_sub" :
        op == "*" ? "bz_decimal_mul" :
        op == "/" ? "bz_decimal_div" : "bz_decimal_mod";
    llvm::Value* oveVt = llvm::ConstantPointerNull::get(ptrTy());
    auto vit = builtinVtable_.find("OverflowException");
    if (vit != builtinVtable_.end()) oveVt = vit->second;
    auto* fn = declareC(fn_name, llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
    builder_.CreateCall(fn, {aSlot, bSlot, oSlot, oveVt});
    emitPendingCheck();
    return builder_.CreateLoad(dst, oSlot, "dec.cassign.res");
}

// A bare %bz.decimal value of 1 (for decimal `++`/`--`).
llvm::Value* Emitter::emitDecimalOne() {
    llvm::AllocaInst* slot = entryAlloca(decimalStructType());
    emitIntToDecimal(llvm::ConstantInt::get(i32(), 1),
                     sema::PrimitiveType::Int, slot);
    return builder_.CreateLoad(decimalStructType(), slot, "dec.one");
}

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
        // §5.2 (#1159): widening a SIGNED source to an UNSIGNED destination is a
        // CHECKED conversion — a negative value is not representable in any
        // unsigned type, so raise OverflowException before sign-extending (which
        // would otherwise silently yield a huge positive value). Mirrors the
        // widening-to-unsigned guard in lowerConversion for the nullable path.
        if (!srcUns && dstUns) {
            llvm::Value* bad = builder_.CreateICmpSLT(
                payload, llvm::ConstantInt::get(srcTy, 0), "s2u.widen.oor");
            emitGuardThrow(bad, "OverflowException");
        }
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
        // §5.2: integer narrowing raises OverflowException if the value does not
        // fit the target's range. Range-check the SOURCE value against the
        // destination's [min, max] using the SOURCE's signedness.
        //
        // #1133: the old "truncate, re-extend with the DEST's signedness, compare
        // bit patterns" round-trip was wrong on a signedness mismatch. For
        // `int(ulong.MaxValue)` the truncated -1 sign-extended back to all-ones,
        // which equals the unsigned source's bit pattern, so the (huge) value
        // falsely passed and yielded -1. A bit-pattern round-trip conflates an
        // unsigned source value with a negative dest value that share low bits.
        llvm::Value* trunc = builder_.CreateTrunc(src, dstTy);
        int64_t dstMax, dstMin;
        // #1322 / §5.1/§5.2: `bool(n)` is now a sema error, so `bool` can
        // never be the destination here.  The `|| db == 1` clause added by
        // #1269 (which worked around bool's unsigned i1 range) is removed as
        // dead code.  Only unsigned numeric destinations (dstUns) now need the
        // unsigned [0, 2^db) range; signed numeric destinations use the signed
        // range.
        if (dstUns) {
            dstMin = 0;
            dstMax = static_cast<int64_t>((1ULL << db) - 1ULL);
        } else {
            dstMin = -static_cast<int64_t>(1ULL << (db - 1));
            dstMax = static_cast<int64_t>((1ULL << (db - 1)) - 1ULL);
        }
        llvm::Value* hiC = llvm::ConstantInt::getSigned(srcTy, dstMax);
        llvm::Value* bad;
        if (srcUns) {
            // An unsigned source is >= 0, so only the upper bound can be
            // exceeded; compare UNSIGNED (dstMax >= 0 always fits srcTy).
            bad = builder_.CreateICmpUGT(src, hiC);
        } else {
            llvm::Value* loC = llvm::ConstantInt::getSigned(srcTy, dstMin);
            bad = builder_.CreateOr(builder_.CreateICmpSLT(src, loC),
                                    builder_.CreateICmpSGT(src, hiC));
        }
        emitGuardThrow(bad, "OverflowException");
        return trunc;
    }
    if (db > sb) {
        // §5.1: bool (i1) widening to any integer always zero-extends
        // (true=1, false=0). Sign-extending i1 would give -1 for true.
        if (srcTy->isIntegerTy(1))
            return builder_.CreateZExt(src, dstTy);
        // §5.2 (#1159): widening a SIGNED source to an UNSIGNED destination is a
        // CHECKED conversion — an explicit integer conversion "preserves the
        // numeric value or fails", and a negative value is not representable in
        // any unsigned type. Without this guard the sign-extension below silently
        // turned a negative source into a huge positive value (e.g. ulong(-1) =
        // 2^64-1). The narrowing path range-checks [0,max] and the same-width
        // path checks the sign bit (#753); this closes the widening-to-unsigned
        // gap. Raise OverflowException when src < 0 before the extension.
        if (!srcUns && dstUns) {
            llvm::Value* bad = builder_.CreateICmpSLT(
                src, llvm::ConstantInt::get(srcTy, 0), "s2u.widen.oor");
            emitGuardThrow(bad, "OverflowException");
        }
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
                // §9.1 / §15.4 (#1129): an INLINE-ARRAY field whose positional
                // argument is a collection literal `[e1, e2, …]` must be built IN
                // PLACE. A CollectionLiteralExpr is not a general expression value
                // — lowerExpr returns null/garbage for it — so the normal
                // lower-then-store path below left the record's array field
                // uninitialized (read-back garbage). Mirror the field-initializer
                // path (Setup.cpp): GEP into the field slot and store each element.
                // (Binding the literal to a named local first already worked
                // because the local-decl path materializes the array.)
                {
                    auto fit = fieldIndex_.find(fields[i]);
                    if (fit != fieldIndex_.end()) {
                        llvm::Type* fty0 = st->getElementType(fit->second);
                        auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                            fieldArg[i]->value.get());
                        if (cl && !cl->isDict) {
                            if (auto* aty =
                                    llvm::dyn_cast<llvm::ArrayType>(fty0)) {
                                // Elements belong to the outer scope — lower them
                                // under the enclosing substitution (issue #597).
                                auto rS = genericSubst_;
                                auto rT = genericSubstSemaTy_;
                                genericSubst_ = argSubst;
                                genericSubstSemaTy_ = argSemaTy;
                                llvm::Value* fieldGep =
                                    builder_.CreateStructGEP(st, slot, fit->second);
                                llvm::Type* elemTy = aty->getElementType();
                                for (std::size_t k = 0; k < cl->elements.size();
                                     ++k) {
                                    if (!cl->elements[k]) continue;
                                    llvm::Value* ev = lowerExpr(*cl->elements[k]);
                                    if (!ev) {
                                        // #1386: ICE — lowerExpr returned null
                                        // for a record construction inline-array
                                        // field element.
                                        ctx_.diagnostics().report(
                                            Severity::Error,
                                            cl->elements[k]->span.begin,
                                            "internal: lowerExpr returned null "
                                            "for record construction inline-array"
                                            " field element — #1386");
                                        continue;
                                    }
                                    if (ev->getType() != elemTy &&
                                        ev->getType()->isIntegerTy() &&
                                        elemTy->isIntegerTy()) {
                                        unsigned eb =
                                            ev->getType()->getIntegerBitWidth();
                                        unsigned db =
                                            elemTy->getIntegerBitWidth();
                                        ev = db > eb
                                                 ? builder_.CreateSExt(ev, elemTy)
                                                 : builder_.CreateTrunc(ev, elemTy);
                                    }
                                    builder_.CreateStore(
                                        ev,
                                        builder_.CreateGEP(
                                            fty0, fieldGep,
                                            {builder_.getInt32(0),
                                             builder_.getInt32(
                                                 static_cast<unsigned>(k))}));
                                }
                                genericSubst_ = std::move(rS);
                                genericSubstSemaTy_ = std::move(rT);
                                continue;
                            }
                        }
                    }
                }
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
                // §5/§6/§9.1: widen/narrow a numeric positional argument to the
                // field's storage type before the store — e.g. an `int` literal
                // `10` (i32) into a `long` field (i64). With opaque pointers
                // `store i32 into ptr` silently writes only the low 4 bytes,
                // leaving the field's upper bits as uninitialized alloca garbage
                // (wrong value / a spurious OverflowException when the garbage is
                // later read). The CLASS positional-field path already coerces;
                // mirror it here. A no-op for the nullable-value struct handled
                // above and for already-matching types.
                llvm::Type* fieldLlvmTy = st->getElementType(idxIt->second);
                v = coerceNumericToType(
                    v, fieldLlvmTy, isUnsignedTargetExpr(*fieldArg[i]->value));
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
        if (!v) {
            // #1386: ICE — lowerExpr returned null for a record field/property
            // default initializer; the expression kind is not yet handled.
            ctx_.diagnostics().report(
                Severity::Error, init->span.begin,
                "internal: lowerExpr returned null for record field/property "
                "default initializer — #1386");
            continue;
        }
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
        // fix #1254 / §19.3 / §14.1: for a destructuring arm over a concrete
        // generic record (e.g. `Wrapper(x) => x.v` where w: Wrapper<Item>),
        // install the concrete type-param substitution so fieldAddr can resolve
        // T-typed member accesses in the arm guard and body. Without this,
        // sema's declType for the bound name `x` is the bare generic param `T`
        // (not the concrete `Item`), so `x.v` finds no sema symbol and
        // fieldAddr returns null — lowerExpr returns null — case.end is left
        // unterminated (IR verification failure). The guard is destroyed at the
        // end of the arm iteration, restoring the outer substitution.
        RecordSubstGuard armSubst(this,
            dynamic_cast<const ast::DestructuringPattern*>(arm.pattern.get())
                ? scrutTy : nullptr);
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

    // §19.3 (v0.183, #1584): a heterogeneous alternative list (AltPattern) —
    // ranges/relationals mixed with constants — lowers as the OR of its
    // members' individual tests. Each member is matched against the same
    // scrutinee, enum/string context, and scrutTy, so a nullable scrutinee
    // threads through per member (each member's own handler ANDs with hasValue,
    // and OR distributes over the shared hasValue). A pure-constants list is
    // collapsed into a ConstantPattern by the parser and never reaches here.
    if (auto* ap = dynamic_cast<const ast::AltPattern*>(pat)) {
        llvm::Value* any = builder_.getInt1(false);
        for (const auto& m : ap->members) {
            if (!m) continue;
            llvm::Value* mm =
                patternMatches(m.get(), scrut, enumTy, isStr, scrutTy);
            if (!mm) return nullptr;
            any = builder_.CreateOr(any, mm);
        }
        return any;
    }

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

    // §19.3 (#1152): a `decimal` scrutinee lowers to decimalStructType() — a
    // 128-bit struct, neither integer nor floating-point. The comparing-pattern
    // forms below (relational, range, constant) branch only on isFloatingPointTy
    // (FCmp) vs else (ICmp); an ICmp on struct operands is illegal IR and aborts
    // the compiler (ICmpInst::AssertOK). Route a decimal scrutinee through the
    // runtime comparison bz_decimal_cmp — the same path a plain `x < 2m` or a
    // `when v < 2m` guard already uses (see lowerBinary, ~L5128). `decCmp`
    // stores both decimal-struct operands into slots, calls bz_decimal_cmp
    // (returns -1/0/+1), and reduces it to the i1 pattern result for the op.
    auto decCmp = [&](llvm::Value* lhs, llvm::Value* rhs,
                      std::string_view op) -> llvm::Value* {
        llvm::StructType* dst = decimalStructType();
        llvm::AllocaInst* aSlot = entryAlloca(dst);
        llvm::AllocaInst* bSlot = entryAlloca(dst);
        builder_.CreateStore(lhs, aSlot);
        builder_.CreateStore(rhs, bSlot);
        auto* cmpFn = declareC("bz_decimal_cmp", i32(), {ptrTy(), ptrTy()});
        llvm::Value* cmp = builder_.CreateCall(cmpFn, {aSlot, bSlot}, "dec.cmp");
        llvm::Value* zero = llvm::ConstantInt::get(i32(), 0);
        if (op == "=")  return builder_.CreateICmpEQ(cmp, zero);
        if (op == "<>") return builder_.CreateICmpNE(cmp, zero);
        if (op == "<")  return builder_.CreateICmpSLT(cmp, zero);
        if (op == "<=") return builder_.CreateICmpSLE(cmp, zero);
        if (op == ">")  return builder_.CreateICmpSGT(cmp, zero);
        /* >= */        return builder_.CreateICmpSGE(cmp, zero);
    };
    // §19.3 (#1152): lower a comparing-pattern operand (a range bound, a
    // relational bound, or a constant) as a `decimal` struct. A decimal LITERAL
    // in pattern position frequently lacks a sema exprType (pattern operands are
    // not value-typechecked the way ordinary expressions are), so the generic
    // lowerExpr path defaults it to `double` (line ~69) — an LLVM type distinct
    // from decimalStructType(), which would feed a double bit-pattern to
    // bz_decimal_cmp. Parse a numeric literal DIRECTLY into the decimal struct
    // (compile-time, lossless — mirrors the FloatLiteralExpr decimal path at
    // ~L49); for a non-literal operand (a named decimal const) lower normally and
    // accept the already-decimal value (or promote an integer).
    auto decOperand = [&](const ast::Expr& e) -> llvm::Value* {
        const ast::Expr* x = &e;
        bool neg = false;
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(x))
            if (un->op == "-" && un->operand) { neg = true; x = un->operand.get(); }
        std::string digits;
        bool isNumLit = false;
        if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(x)) {
            isNumLit = true;
            for (char c : fl->text)
                if (c != '_' && c != 'm' && c != 'M') digits.push_back(c);
        } else if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(x)) {
            isNumLit = true;
            for (char c : il->text)
                if (c != '_' && c != 'm' && c != 'M') digits.push_back(c);
        }
        if (isNumLit) {
            if (neg) digits.insert(digits.begin(), '-');  // sign via the parser
            uint32_t d[4] = {0, 0, 0, 0};
            bz_decimal_parse_cstr(digits.c_str(), d, nullptr, nullptr);
            llvm::StructType* dst = decimalStructType();
            return llvm::ConstantStruct::get(dst, {
                llvm::ConstantInt::get(i32(), d[0]),
                llvm::ConstantInt::get(i32(), d[1]),
                llvm::ConstantInt::get(i32(), d[2]),
                llvm::ConstantInt::get(i32(), d[3])});
        }
        // Non-literal operand: lower normally and coerce to a decimal struct.
        llvm::Value* v = lowerExpr(e);
        if (!v) return nullptr;
        if (v->getType() == decimalStructType()) return v;  // already decimal
        if (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(1)) {
            llvm::AllocaInst* slot = entryAlloca(decimalStructType());
            emitIntToDecimal(v, sema::PrimitiveType::Int, slot);  // §5.1 widen
            return builder_.CreateLoad(decimalStructType(), slot, "dec.op");
        }
        return v;  // last resort (should not occur for a decimal operand)
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
            // §19.5 (#1128): a bare TYPE-name pattern on a nullable record/enum
            // scrutinee (`Pt?` matched by `Pt`, `Color?` by `Color`) is a
            // type-test, not a constant. Records and enums are sealed VALUE
            // types with no subtyping (§9.6 / §10), so the bare name matches
            // exactly its own type and — per §19.5 ("a bare type pattern T
            // matches only non-null values") — only when the value is present
            // (hasValue). Without this the bare type name (whose last part is
            // not an enum constant) fell through to the deferred `return
            // nullptr` below → lowerCaseExpr/Stmt left `case.end` unterminated
            // (expr-position IR-verify crash) or silently skipped the arm and
            // every later arm (stmt-position wrong value).
            if (np->name && !np->name->parts.empty() && scrutTy->declaration &&
                np->name->parts.back() == scrutTy->declaration->name)
                return hasVal;
            return nullptr;  // type-test pattern deferred
        }

        // §19: ConstantPattern on a nullable value — hasValue AND value == c.
        if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(pat)) {
            llvm::Value* any = builder_.getInt1(false);
            for (const auto& c : cp->constants) {
                if (!c) continue;
                // §19.5 (#1471): a `null` member folded into a `|`-alternative
                // over a nullable VALUE scrutinee (`1 | null`, `Red | null`,
                // `true | null`) lands here as a NullLiteralExpr, not as a
                // standalone NullPattern, so the early NullPattern return above
                // is never reached. Route it through the same has-no-value test:
                // `null` matches exactly when hasValue == 0. Lowering it as a
                // value instead produced a pointer (ConstantPointerNull) that
                // emitEquals fed to an ICmp against the unwrapped inner value —
                // an ICmp on mismatched types that aborted the backend.
                if (dynamic_cast<const ast::NullLiteralExpr*>(c.get())) {
                    llvm::Value* isNull =
                        builder_.CreateNot(hasVal, "nv.null.match");
                    any = builder_.CreateOr(any, isNull);
                    continue;
                }
                // §19.3 (#1152): a `decimal?` value compares via bz_decimal_cmp;
                // emitEquals would ICmp two 128-bit structs (illegal IR / abort).
                // decOperand lowers the constant as a decimal (a decimal literal
                // lacking a sema exprType would otherwise default to double).
                if (innerVal->getType() == decimalStructType()) {
                    llvm::Value* dop = decOperand(*c);
                    if (!dop) return nullptr;
                    llvm::Value* match = builder_.CreateAnd(
                        hasVal, decCmp(innerVal, dop, "="), "nv.val.match");
                    any = builder_.CreateOr(any, match);
                    continue;
                }
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
        // vacuously satisfied.) §19.3/§19.5 (#1472): a bare `..` (no lo/hi/step,
        // grammar A.7) has both bounds absent, so geL and leH both default to
        // true and the result reduces to `hasValue` — a range pattern matches
        // every PRESENT value but never null (§19.5: null is matched only by
        // `null`/untyped `var x`/`_`). The guard therefore admits the both-
        // bounds-absent case (previously excluded by `(lo || hi)`, which left
        // `case.end` unterminated / the arm silently skipped).
        if (auto* rp = dynamic_cast<const ast::RangePattern*>(pat)) {
            auto* range =
                dynamic_cast<const ast::RangeExpr*>(rp->rangeExpr.get());
            if (range && !range->step) {
                bool fp = innerVal->getType()->isFloatingPointTy();
                // §19.3 (#1152): a `decimal?` value compares via bz_decimal_cmp;
                // bounds are lowered via decOperand (a decimal literal lacking a
                // sema exprType would otherwise default to double).
                bool dec = innerVal->getType() == decimalStructType();
                llvm::Value* geL = builder_.getInt1(true);
                if (range->lo) {
                    llvm::Value* lo = dec ? decOperand(*range->lo)
                                          : litAtWidth(*range->lo, innerVal->getType());
                    if (!lo && !dec) lo = reconcile(lowerExpr(*range->lo),
                                                    innerVal->getType());  // #836
                    if (!lo) return nullptr;
                    geL = dec ? decCmp(innerVal, lo, ">=")
                              : fp ? builder_.CreateFCmpOGE(innerVal, lo)
                                   : (scrutUnsigned ? builder_.CreateICmpUGE(innerVal, lo)
                                                    : builder_.CreateICmpSGE(innerVal, lo));
                }
                llvm::Value* leH = builder_.getInt1(true);
                if (range->hi) {
                    llvm::Value* hi = dec ? decOperand(*range->hi)
                                          : litAtWidth(*range->hi, innerVal->getType());
                    if (!hi && !dec) hi = reconcile(lowerExpr(*range->hi),
                                                    innerVal->getType());  // #836
                    if (!hi) return nullptr;
                    leH = dec
                        ? decCmp(innerVal, hi, range->inclusive ? "<=" : "<")
                        : fp
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

        // §19.3 (#1074): a relational pattern (`> 3`, `<= n`, …) on a nullable
        // value — hasValue AND the unwrapped value satisfies the comparison. A
        // null scrutinee never matches a relational pattern. Mirrors the
        // non-nullable RelOpPattern handler below, applied to innerVal. Without
        // this, a relational pattern on a nullable scrutinee fell through to the
        // deferred null return → lowerCaseExpr bailed leaving `case.end`
        // unterminated (the malformed-IR / verification crash).
        if (auto* rp = dynamic_cast<const ast::RelOpPattern*>(pat)) {
            // §19.3 (#1152): a `decimal?` value compares via bz_decimal_cmp;
            // ICmp on the 128-bit struct operands would abort the compiler.
            // decOperand lowers the bound as a decimal (a decimal literal lacking
            // a sema exprType would otherwise default to double).
            if (innerVal->getType() == decimalStructType()) {
                llvm::Value* dop = rp->expr ? decOperand(*rp->expr) : nullptr;
                if (!dop) return nullptr;
                return builder_.CreateAnd(hasVal, decCmp(innerVal, dop, rp->op),
                                          "nv.relop.match");
            }
            llvm::Value* rv =
                rp->expr ? litAtWidth(*rp->expr, innerVal->getType()) : nullptr;
            if (!rv && rp->expr)
                rv = reconcile(lowerExpr(*rp->expr), innerVal->getType());
            if (!rv) return nullptr;
            bool fp = innerVal->getType()->isFloatingPointTy();
            llvm::Value* cmp = nullptr;
            if (rp->op == "<")
                cmp = fp ? builder_.CreateFCmpOLT(innerVal, rv)
                         : (scrutUnsigned ? builder_.CreateICmpULT(innerVal, rv)
                                          : builder_.CreateICmpSLT(innerVal, rv));
            else if (rp->op == ">")
                cmp = fp ? builder_.CreateFCmpOGT(innerVal, rv)
                         : (scrutUnsigned ? builder_.CreateICmpUGT(innerVal, rv)
                                          : builder_.CreateICmpSGT(innerVal, rv));
            else if (rp->op == "<=")
                cmp = fp ? builder_.CreateFCmpOLE(innerVal, rv)
                         : (scrutUnsigned ? builder_.CreateICmpULE(innerVal, rv)
                                          : builder_.CreateICmpSLE(innerVal, rv));
            else if (rp->op == ">=")
                cmp = fp ? builder_.CreateFCmpOGE(innerVal, rv)
                         : (scrutUnsigned ? builder_.CreateICmpUGE(innerVal, rv)
                                          : builder_.CreateICmpSGE(innerVal, rv));
            if (!cmp) return nullptr;
            return builder_.CreateAnd(hasVal, cmp, "nv.relop.match");
        }

        // #887 / §19.3: a record DESTRUCTURING pattern `T(x, y, ...)` on a
        // nullable record `T?` matches only when the value is present. Extract
        // the unwrapped record value (nullablePrimValue → element 1) and run the
        // non-nullable destructuring extraction on it — which binds the field
        // slots from the record's fields — then AND with hasValue. Passing a null
        // scrutTy on the recursion takes the non-nullable path, reaching the
        // DestructuringPattern handler below with the record struct as scrutinee.
        // Without this the pattern fell through to the "deferred" null return
        // below, and the case lowering then emitted an unterminated basic block
        // (IR verification failure).
        if (dynamic_cast<const ast::DestructuringPattern*>(pat)) {
            llvm::Value* sub =
                patternMatches(pat, innerVal, enumTy, isStr, /*scrutTy=*/nullptr);
            if (!sub) return nullptr;
            return builder_.CreateAnd(hasVal, sub, "nv.destr.match");
        }

        // §19.5 (#1128): a bare TYPE pattern `T` on a nullable VALUE-type
        // scrutinee `T?` (e.g. `int` on `int?`) is a type-test. Per §19.5 "a
        // bare type pattern T matches only non-null values", so it matches
        // exactly when the value is present (hasValue) AND the pattern type is
        // the scrutinee's underlying value type. The scrutinee is the unboxed
        // tagged struct {i8 hasValue; T value}, so the static types already
        // agree and the test reduces to `hasValue`; a pattern type that differs
        // from the underlying value type can never match. Without this the
        // pattern fell through to the deferred `return nullptr` below →
        // lowerCaseExpr/Stmt left `case.end` unterminated (expr-position
        // IR-verify crash) or silently skipped the arm and every later arm
        // (stmt-position wrong value).
        if (auto* tp = dynamic_cast<const ast::TypePattern*>(pat)) {
            const sema::TypeRef* patTy =
                tp->type ? ctx_.resolvedType(*tp->type) : nullptr;
            bool sameUnderlying = false;
            if (patTy) {
                if (patTy->kind == sema::TypeRef::Kind::Primitive &&
                    scrutTy->kind == sema::TypeRef::Kind::Primitive)
                    sameUnderlying = patTy->primitive == scrutTy->primitive;
                else if (patTy->kind == sema::TypeRef::Kind::Named &&
                         scrutTy->kind == sema::TypeRef::Kind::Named)
                    sameUnderlying = patTy->declaration == scrutTy->declaration;
            }
            return sameUnderlying ? hasVal : builder_.getInt1(false);
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
        // §19 / §A.8 (#957): a NamedPattern whose qualified name resolves to a
        // named CONST (`C.K`, a module/own-class const) is a constant-equality
        // pattern, not a type test — the parser emits a NamedPattern for any
        // non-primitive name and defers Constant-vs-Type to resolution here.
        // Fold the const's value and compare (mirrors the ConstantPattern
        // handler: reconcile width for non-string scrutinees, content-compare
        // for strings). Checked before the type-descriptor branch; the two
        // never collide (a const member's last name part is not a type name).
        if (llvm::Value* cv = constValueForPatternName(np->name.get())) {
            if (!isStr) cv = reconcile(cv, scrut->getType());
            // §19 (#964): a named-const pattern whose type is incompatible with
            // the scrutinee (e.g. a `string` const vs an `int` scrutinee) would
            // emit an `ICmp`/content-compare over mismatched LLVM types — a hard
            // abort (ICmpInst::AssertOK). `reconcile` bridges only int/fp widths;
            // a residual ptr-vs-scalar, i1-vs-wider-int, or fp-vs-int mismatch is
            // a §19 type error. (Sema rejects the literal-ConstantPattern form up
            // front; a NamedPattern's const type is not resolved in sema, so this
            // is the backstop.) Report it cleanly and never-match rather than
            // crash.
            llvm::Type* st = scrut->getType();
            llvm::Type* ct = cv->getType();
            bool incompatible =
                st != ct &&
                (st->isPointerTy() != ct->isPointerTy() ||
                 st->isIntegerTy(1) != ct->isIntegerTy(1) ||
                 st->isFloatingPointTy() != ct->isFloatingPointTy());
            if (incompatible) {
                ctx_.diagnostics().report(
                    Severity::Error, np->span.begin,
                    "case pattern constant type is not compatible with the "
                    "scrutinee type (\xC2\xA7""19) — a `case` constant must be "
                    "comparable to the scrutinee");
                return builder_.getInt1(false);
            }
            return emitEquals(scrut, cv, isStr);
        }
        // §19.3/§19.5: type-test pattern on a class/interface scrutinee.
        if (llvm::GlobalVariable* desc =
                descriptorForPatternName(np->name.get())) {
            // sweep #7: a class/interface type-test only matches a heap object
            // that carries a vtable/descriptor (a class, an interface, or
            // Object). Against a value-type primitive (e.g. an `int` scrutinee →
            // emitIsA compares an i32 to a ptr → LLVM SIGABRT) or a runtime-
            // managed reference like `string` (→ emitIsA loads a vtable from a
            // non-object → SIGSEGV), the test can NEVER match — return false.
            bool objectLike = !scrutTy ||
                (scrutTy->kind == sema::TypeRef::Kind::Named &&
                 scrutTy->declaration &&
                 (scrutTy->declaration->kind == ast::TypeDeclKind::Class ||
                  scrutTy->declaration->kind == ast::TypeDeclKind::Interface)) ||
                (scrutTy->kind == sema::TypeRef::Kind::Primitive &&
                 scrutTy->primitive == sema::PrimitiveType::Object);
            if (!scrut->getType()->isPointerTy() || !objectLike)
                return builder_.getInt1(false);
            return emitIsA(scrut, desc);
        }
        // §19.5 (#1206): a bare type-name pattern whose name resolves to a
        // RECORD or ENUM — a sealed value type with no subtyping — is
        // statically decidable.  Records are struct values (not pointers) and
        // enums are i32 values; neither carries a vtable/descriptor, so the
        // runtime emitIsA path (above) is wrong for them.  Decide at compile
        // time: if the scrutinee's static type IS the named record/enum the arm
        // always matches; any different concrete type means never-match.
        // A pointer-typed scrutinee (e.g. an Object holding a boxed record)
        // cannot syntactically reach here with a bare value-type name pattern —
        // sema would reject it — but guard it anyway.  Mirrors the BindingPattern
        // typed-binding branch (L10916-10923).  per §19.5 (#1206)
        if (np->name && !np->name->parts.empty()) {
            std::string_view last = np->name->parts.back();
            for (const auto& sym : ctx_.symbols()) {
                auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
                if (!ts || ts->name != last) continue;
                if (ts->kind == ast::TypeDeclKind::Record ||
                    ts->kind == ast::TypeDeclKind::Enum) {
                    if (scrut->getType()->isPointerTy())
                        return builder_.getInt1(false);
                    bool sameType =
                        scrutTy && scrutTy->declaration == ts;
                    return builder_.getInt1(sameType);
                }
            }
        }
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
    if (auto* tp = dynamic_cast<const ast::TypePattern*>(pat)) {
        // sweep #7 [4]: a primitive type pattern (`string`, `int`, …) against a
        // scrutinee of a DIFFERENT concrete primitive type can NEVER match — e.g.
        // a `string` pattern on an `int` scrutinee. The unconditional match below
        // wrongly fired such an arm (a wrong value). When both the pattern and the
        // (non-nullable, non-Object) scrutinee are concrete primitives that
        // differ, return false; an Object/boxed scrutinee keeps the runtime path.
        if (tp->type && scrutTy &&
            scrutTy->kind == sema::TypeRef::Kind::Primitive &&
            scrutTy->primitive != sema::PrimitiveType::Object) {
            const sema::TypeRef* patTy = ctx_.resolvedType(*tp->type);
            if (patTy && patTy->kind == sema::TypeRef::Kind::Primitive &&
                patTy->primitive != sema::PrimitiveType::Object &&
                patTy->primitive != scrutTy->primitive)
                return builder_.getInt1(false);
        }
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
            // §19.3 (#1152): a decimal scrutinee compares via bz_decimal_cmp;
            // emitEquals would ICmp two 128-bit structs (illegal IR / abort).
            // The constant is lowered via decOperand so a decimal literal that
            // lacks a sema exprType does not default to double.
            if (scrut->getType() == decimalStructType()) {
                llvm::Value* dop = decOperand(*c);
                if (!dop) return nullptr;
                any = builder_.CreateOr(any, decCmp(scrut, dop, "="));
                continue;
            }
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
        // §19.3 (#1152): a decimal scrutinee compares via bz_decimal_cmp; ICmp
        // on the 128-bit struct operands would abort the compiler. decOperand
        // lowers the bound as a decimal (a decimal literal that lacks a sema
        // exprType would otherwise default to double).
        if (scrut->getType() == decimalStructType()) {
            llvm::Value* dop = rp->expr ? decOperand(*rp->expr) : nullptr;
            if (!dop) return nullptr;
            return decCmp(scrut, dop, rp->op);
        }
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
        // lower (leH = true). §19.3 / grammar A.7 (#1472): a bare `..` pattern
        // (no lo/hi/step) has BOTH bounds absent, so geL and leH both default to
        // true and the result reduces to true — an unconditional match, matching
        // every value (equivalently `_`). This case was previously excluded by
        // the `(lo || hi)` clause, contradicting the comment, so `patternMatches`
        // returned nullptr → an unterminated `case.end` block (expr-position IR-
        // verification crash) or a silently-never-taken arm (stmt position).
        if (range && !range->step) {
            bool fp = scrut->getType()->isFloatingPointTy();
            // §19.3 (#1152): a decimal scrutinee compares via bz_decimal_cmp;
            // ICmp on the 128-bit struct operands would abort the compiler. The
            // bounds are lowered via decOperand so a decimal literal that lacks
            // a sema exprType does not default to double.
            bool dec = scrut->getType() == decimalStructType();
            // §19.3: range patterns — use FP comparisons for float scrutinees.
            llvm::Value* geL = builder_.getInt1(true);
            if (range->lo) {
                llvm::Value* lo = dec ? decOperand(*range->lo)
                                      : litAtWidth(*range->lo, scrut->getType());  // #836
                if (!lo && !dec) lo = reconcile(lowerExpr(*range->lo), scrut->getType());
                if (!lo) return nullptr;
                geL = dec ? decCmp(scrut, lo, ">=")
                          : fp ? builder_.CreateFCmpOGE(scrut, lo)
                               : (scrutUnsigned ? builder_.CreateICmpUGE(scrut, lo)
                                                : builder_.CreateICmpSGE(scrut, lo));
            }
            llvm::Value* leH = builder_.getInt1(true);
            if (range->hi) {
                llvm::Value* hi = dec ? decOperand(*range->hi)
                                      : litAtWidth(*range->hi, scrut->getType());  // #836
                if (!hi && !dec) hi = reconcile(lowerExpr(*range->hi), scrut->getType());
                if (!hi) return nullptr;
                leH = dec
                    ? decCmp(scrut, hi, range->inclusive ? "<=" : "<")
                    : fp
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
                    // §3.3 per-instantiation (#1473): inside a monomorphized
                    // generic body a type-parameter scrutinee's exprType is
                    // still the placeholder param ref (Kind::Named, no
                    // declaration). Resolve it to the concrete substituted type
                    // so the same-primitive test reflects the actual
                    // instantiation — `var b: int` matches when T=int but not
                    // when T=char. For a concrete scrutinee resolveTypeParamRef
                    // is the identity, so this does not disturb the concrete
                    // paths below.
                    const sema::TypeRef* rScrutTy = resolveTypeParamRef(scrutTy);
                    bool scrutIsSamePrim =
                        rScrutTy &&
                        rScrutTy->kind == sema::TypeRef::Kind::Primitive &&
                        rScrutTy->primitive == rt->primitive &&
                        !rScrutTy->nullable;
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
                    // §19.3/§19.5 (#1473): a typed binding `var x: P` where P is
                    // a primitive is a type-test — it matches iff the
                    // scrutinee's runtime type IS P. This unconditional return
                    // was only sound for the statically-satisfied case (the
                    // scrutinee's resolved static type already IS P). A
                    // MISMATCHED value-type primitive (e.g. an `int` scrutinee
                    // vs `var b: bool`, or a T=char instantiation vs `var b:
                    // int`) can NEVER match; returning unconditional `true` here
                    // bound the scrutinee's raw bits under the wrong type and
                    // selected the wrong arm (unsound — wrong runtime value).
                    // Gate on scrutIsSamePrim so the mismatch falls through to
                    // the next / `_` arm, mirroring the bare type-pattern form
                    // (`bool` over an `int` scrutinee already yields no match).
                    return builder_.getInt1(scrutIsSamePrim);
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
    // keyed on the naked binding's BindingPattern anchor (#1271:
    // `sp.nakedAnchor.get()`), mirroring the sema anchor (`lsym->decl =
    // sp.nakedAnchor.get()`), so `localSlot()` resolves the identifier to the
    // right slot.
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
        // fix #1254 / §19.3 / §14.1: for a concrete generic-record scrutinee
        // (e.g. Wrapper<Item>), install the concrete type-param substitution
        // (T→ptr for Item) so recordStruct returns the specialized layout and
        // field GEPs use the correct concrete element types. Without this the
        // unspecialized placeholder struct (T→i32) is used, which can mismatch
        // the actual runtime struct layout for non-int type arguments.
        RecordSubstGuard destrSubst(this, scrutTy);
        // Ensure the record struct (and fieldIndex_) is populated.
        llvm::StructType* recSt = recordStruct(recTy);
        // Collect positional (non-static) fields in declaration order.
        std::vector<const sema::FieldSymbol*> fields;
        for (const sema::Symbol* m : recTy->members) {
            if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m)) {
                if (!isStaticField(fs)) fields.push_back(fs);
            }
        }
        // #1234 / §19 / §9.4: when the scrutinee is a REFERENCE (Object or
        // interface — `scrut` is a box pointer), the record is heap-boxed;
        // `CreateExtractValue` on a pointer is illegal IR and aborts the
        // backend.  Emit an isA type-test to confirm the box holds a `recTy`,
        // then conditionally load the record struct from the box payload (at
        // offset 2 pointer-sized words past the box header, per the §8.6 box
        // layout { vtable*, rc, record-bytes }) and run the sub-pattern checks
        // on the loaded struct value.
        if (scrut->getType()->isPointerTy()) {
            auto dit = descArray_.find(recTy);
            if (dit == descArray_.end()) return builder_.getInt1(false);
            // isA check — emitIsA leaves builder insert point at isa.end.
            llvm::Value* isA = emitIsA(scrut, dit->second);
            llvm::BasicBlock* afterIsaBB = builder_.GetInsertBlock();
            auto* unboxBB =
                llvm::BasicBlock::Create(llvm_, "destr.unbox", currentFn_);
            auto* joinBB =
                llvm::BasicBlock::Create(llvm_, "destr.join", currentFn_);
            builder_.CreateCondBr(isA, unboxBB, joinBB);
            // Unbox: load the record struct from the box payload (2 words in).
            builder_.SetInsertPoint(unboxBB);
            llvm::Value* payload = builder_.CreateGEP(
                ptrTy(), scrut, {builder_.getInt32(2)}, "destr.payload");
            llvm::Value* innerVal =
                builder_.CreateLoad(recSt, payload, "destr.inner");
            // Field extraction and sub-pattern checks on the loaded struct.
            llvm::Value* innerMatched = builder_.getInt1(true);
            for (std::size_t i = 0; i < dp->subPatterns.size(); ++i) {
                const auto& sp = dp->subPatterns[i];
                if (i >= fields.size()) {
                    innerMatched = builder_.getInt1(false); break;
                }
                auto idxIt = fieldIndex_.find(fields[i]);
                if (idxIt == fieldIndex_.end()) {
                    innerMatched = builder_.getInt1(false); break;
                }
                llvm::Value* fv =
                    builder_.CreateExtractValue(innerVal, {idxIt->second});
                if (!sp.nakedBinding.empty()) {
                    llvm::AllocaInst* slot = entryAlloca(fv->getType());
                    builder_.CreateStore(fv, slot);
                    locals_[sp.nakedAnchor.get()] = slot;  // #1271 real Node anchor
                } else if (auto* vbp = dynamic_cast<const ast::BindingPattern*>(
                               sp.pattern.get())) {
                    llvm::AllocaInst* slot = entryAlloca(fv->getType());
                    builder_.CreateStore(fv, slot);
                    locals_[vbp] = slot;
                } else if (sp.pattern) {
                    const sema::TypeRef* fieldTy = recordMemberType(fields[i]);
                    const sema::TypeSymbol* subEnumTy = nullptr;
                    bool subIsStr = false;
                    if (fieldTy &&
                        fieldTy->kind == sema::TypeRef::Kind::Named &&
                        fieldTy->declaration &&
                        fieldTy->declaration->kind == ast::TypeDeclKind::Enum)
                        subEnumTy = fieldTy->declaration;
                    else if (fieldTy &&
                             fieldTy->kind == sema::TypeRef::Kind::Primitive &&
                             fieldTy->primitive ==
                                 sema::PrimitiveType::String)
                        subIsStr = true;
                    llvm::Value* sub = patternMatches(
                        sp.pattern.get(), fv, subEnumTy, subIsStr, fieldTy);
                    if (sub) innerMatched = builder_.CreateAnd(innerMatched, sub);
                }
            }
            llvm::BasicBlock* unboxEnd = builder_.GetInsertBlock();
            builder_.CreateBr(joinBB);
            builder_.SetInsertPoint(joinBB);
            auto* phi = builder_.CreatePHI(i1(), 2);
            phi->addIncoming(builder_.getInt1(false), afterIsaBB);
            phi->addIncoming(innerMatched, unboxEnd);
            return phi;
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
                const auto* key = sp.nakedAnchor.get();  // #1271 real Node anchor
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

// per §27.4 (v0.183, #1408) — see declaration (LlvmBackend_impl.hpp) for the
// contract. Resolves the record TypeSymbol the same way patternMatches'
// DestructuringPattern branch does (by name over ctx_.symbols(), just above);
// there is no sema-attached back-pointer on the AST node to reuse.
void Emitter::bindLambdaDestructureParam(const ast::DestructuringPattern& dp,
                                         llvm::Value* structVal) {
    if (!dp.typeName || !structVal) return;
    std::string_view lastName = dp.typeName->parts.empty()
        ? std::string_view{} : dp.typeName->parts.back();
    const sema::TypeSymbol* recTy = nullptr;
    for (const auto& sym : ctx_.symbols()) {
        if (auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get())) {
            if (ts->name == lastName && ts->kind == ast::TypeDeclKind::Record) {
                recTy = ts; break;
            }
        }
    }
    if (!recTy) return;
    recordStruct(recTy);  // ensure fieldIndex_ is populated for this record
    std::vector<const sema::FieldSymbol*> fields;
    for (const sema::Symbol* m : recTy->members) {
        if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m)) {
            if (!isStaticField(fs)) fields.push_back(fs);
        }
    }
    for (std::size_t i = 0; i < dp.subPatterns.size() && i < fields.size(); ++i) {
        const auto& sp = dp.subPatterns[i];
        auto idxIt = fieldIndex_.find(fields[i]);
        if (idxIt == fieldIndex_.end()) continue;
        llvm::Value* fieldVal =
            builder_.CreateExtractValue(structVal, {idxIt->second});
        if (!sp.nakedBinding.empty()) {
            // per §27.4: "each binding is an ordinary read of the
            // corresponding positional field at body entry."
            llvm::AllocaInst* slot = entryAlloca(fieldVal->getType());
            builder_.CreateStore(fieldVal, slot);
            locals_[sp.nakedAnchor.get()] = slot;  // #1271 real Node anchor
        } else if (auto* nested = dynamic_cast<const ast::DestructuringPattern*>(
                       sp.pattern.get())) {
            // Nested LambdaDestructure — recurse on the extracted field.
            bindLambdaDestructureParam(*nested, fieldVal);
        }
        // `_` (DiscardPattern) binds nothing — nothing to do.
    }
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
