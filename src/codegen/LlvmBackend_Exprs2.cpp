#include "codegen/LlvmBackend_impl.hpp"
#include "sema/GenericParams.hpp"  // fix #3143: the ONE both-sources generic-param walk (§14.1/§12.9/§2.5)

#include "common/OwnershipFacts.hpp"  // #2098: bindClauseNeedsAtomDisposal (§17.15/K8.5)
#include "sema/EnumValues.hpp"  // enumDeclaredValues — §5.4 checked cast (#3044)

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

// fix #2805 (§17.7 short-circuit operators / §8.6, v0.240): an operand that
// FAILED TO LOWER must degrade to a well-formed `false`, not to a null that
// poisons the whole chain (and, for the right operand, not to an unterminated
// basic block).
//
// Both halves were live. `lowerExpr` returns null for a DROPPED type test —
// #2775's mechanism, e.g. a value type §8.6 requires to box that codegen does
// not box yet. Before this fix:
//   * a null LEFT operand made the entire `and`/`or` null, which `lowerIf`
//     then turns into a silent `false`. Measured: `o is [3]int or oi is int`
//     answered FALSE with a TRUE disjunct, and `not (o is [3]int) and true`
//     answered FALSE where `true` is required.
//   * a null RIGHT operand returned after `rhsBB` was already branched into,
//     leaving `sc.rhs` and `sc.end` with no terminator — a hard
//     `IR verification failed`, i.e. the compiler aborting on the operand
//     position rather than the value.
// So the same underlying condition produced a silent wrong value in one
// position and a compiler abort in another; neither is acceptable.
//
// `false` is the correct degrade, not an arbitrary one: it is exactly what a
// dropped test already reads as in the single-test case (`lowerIf` branches
// past the body), so this makes the chain CONSISTENT with the unchained
// spelling instead of accidentally right in one shape and wrong in the others.
// The producer is responsible for the diagnostic — lowerTypeTest's four bails
// report #2775 — and the guard below is the backstop for a producer that
// reports nothing, so the degrade is never silent.
//
// ⚠ NOT applied when `diverged_` is set: there the operand THREW, the block is
// already terminated, and synthesising a value would emit code after a
// terminator. That path keeps returning null, which is its established
// contract.
llvm::Value* Emitter::degradedBoolOperand(
        std::initializer_list<const ast::Expr*> locCandidates,
        const char* what) {
    // A synthesized node can carry an empty span (`line` is 1-based, so 0 means
    // unset — the #2737 shape) and renders as a useless `file:0:0`. Both the
    // operand AND its enclosing operator were observed unset on a chained
    // `a and b and c`, so take the first candidate that names a real position
    // rather than assuming any single node has one.
    SourceLocation at{};
    for (const ast::Expr* e : locCandidates)
        if (e && e->span.begin.line != 0) { at = e->span.begin; break; }
    if (!ctx_.diagnostics().hasErrors())
        ctx_.diagnostics().report(
            Severity::Warning, at,
            std::string("internal: the ") + what +
                " failed to lower to a value and was treated as `false` "
                "— #2805");
    return builder_.getInt1(false);
}


llvm::Value* Emitter::lowerShortCircuit(const ast::BinaryExpr& bin) {
    bool isAnd = (bin.op == "and");
    llvm::Value* l = lowerExpr(*bin.left);
    if (!l) {
        if (diverged_) return nullptr;   // the operand threw; block terminated
        l = degradedBoolOperand({bin.left.get(), &bin, bin.right.get()},
                                "left operand of a short-circuit operator");
    }
    // fix #5137 (§7.11, v0.313): the §7.11 'no three-valued logic' gate at
    // `and`/`or` (src/sema/ExprTypecheck_Exprs.cpp ~6925) admits a `bool?`
    // operand narrowed non-null by §7.4. Sema returns success and codegen
    // receives the operand as the declared `{i8 hasVal; i1 payload}` struct,
    // but the short-circuit branch needs an `i1` — extract the payload here.
    // The pre-extraction soundness precondition of unwrapNarrowedNullableOperand
    // (a sema gate that rejects the un-narrowed form) is satisfied by the
    // #5137 sema half in this same lane.
    l = unwrapNarrowedNullableOperand(l, bin.left.get());
    // §22.4 / §7.9 (v0.235, #2395): pre-create/zero-init any inline
    // `out var` slot inside the RIGHT operand — the one THIS operator may
    // SKIP (`and` skips it when `l` is false, `or` skips it when `l` is
    // true) — before the branch below. Without this, ensureOutVarSlots'
    // zero-store (added by this same fix) runs from inside `rhsBB`, which
    // does not dominate `sc.end`'s "skip" predecessor, so a read past the
    // `if` on the skip path still observes stack garbage instead of the
    // §7.9 default. `bin.left` needs no equivalent hoist: it is never
    // skipped by this operator, so its own (possibly nested) lowering
    // reaches any out-var call inside it unconditionally already.
    if (bin.right) hoistSkippableOutVarSlots(*bin.right);
    auto* entryBB = builder_.GetInsertBlock();
    auto* rhsBB = llvm::BasicBlock::Create(llvm_, "sc.rhs", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "sc.end", currentFn_);
    // and: evaluate RHS only when l is true; or: only when l is false.
    if (isAnd) builder_.CreateCondBr(l, rhsBB, endBB);
    else       builder_.CreateCondBr(l, endBB, rhsBB);

    builder_.SetInsertPoint(rhsBB);
    llvm::Value* r = lowerExpr(*bin.right);
    if (!r) {
        // #2805: returning null HERE is what left `sc.rhs`/`sc.end`
        // unterminated — `rhsBB` has already been branched into above, so
        // there is no longer a null-safe exit from this function.
        if (diverged_) return nullptr;   // threw: rhsBB already terminated
        r = degradedBoolOperand({bin.right.get(), &bin, bin.left.get()},
                                "right operand of a short-circuit operator");
    }
    // fix #5137: same extraction for the RHS — the right operand of `and`/`or`
    // is also admitted by the §7.11 gate when narrowed, and the PHI below
    // needs an `i1`.
    r = unwrapNarrowedNullableOperand(r, bin.right.get());
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
            // fix #5109 (§5.1 / §14.1, v0.307): the check above resolves the
            // concrete substitution ONLY when `genericSubstSemaTy_` has a
            // binding for T — true in a MONOMORPHIZED (specialized) generic
            // body, but the UNSPECIALIZED TEMPLATE version of the same method
            // (emitted and IR-VERIFIED alongside the specialization even when
            // never called — every generic method/constructor keeps its
            // erased-placeholder template) compiles this same `x ?? d` with NO
            // active substitution: `nullableTypeParamConcrete` legitimately
            // returns false there, `effectiveLt` stays the bare unresolved
            // `T?`, `isNullableValueTy(effectiveLt)` is false, and execution
            // fell to the "#1045 non-pointer, non-nullable-value LHS" arm below
            // — WRONG for this shape, because a bare value-type generic
            // parameter's nullable form `T?` still lowers to the tagged
            // `{i8 hasValue; T}` struct even in the erased template (T erased
            // to its i32 placeholder), so `l` IS the tagged struct, not a
            // "can never be null" scalar. The #1045 arm then returned the RAW
            // struct as the `??` result, and a caller expecting a scalar (e.g.
            // `Console.WriteLine(x ?? d)`'s argument-to-string conversion, which
            // has no arm for a bare-generic-typed struct value) passed the
            // struct straight to `bz_string_to_utf8z` — a malformed call the IR
            // verifier rejects for the TEMPLATE FUNCTION, failing the whole
            // module's verification even though the template is never called by
            // a program that only invokes the (correctly-lowered) specialization.
            //
            // Recognize the nullable-value carrier STRUCTURALLY instead of only
            // via sema resolution: `nullablePrimHasValue`/`nullablePrimValue`
            // are pure `extractvalue`s on field 0/1 and need no concrete sema
            // type to be correct, so a 2-field `{i8, X}` struct whose first
            // field is the hasValue tag type is unwrapped the same way whether
            // or not `T` is concretely known.
            //
            // GUARDED to the RHS ALSO being an unresolved bare generic `T`
            // (e.g. `d: T`, matching #5109's own shape): both operands then
            // share the SAME erased placeholder type in the template (i32),
            // so the unwrap-and-PHI below is type-safe there too. Measured
            // (#5089 blast radius): dropping this guard broke
            // `nullable_lift_generic_ctor_5089` — `x ?? -1.0` inside a
            // `Box<T>` constructor has a CONCRETE-typed RHS (`double`, not
            // `T`), so the template's erased `i32` payload and the literal
            // `double` -1.0 built a genuinely mismatched PHI ("PHI node
            // operands are not the same type as the result! %8 = phi i32
            // [ %nv.val, ... ], [ -1.000000e+00, %coal.rhs ]") that the old
            // "#1045 return l" fallback never reached (it never lowers the
            // RHS). That template is still IR-verified though never called;
            // the pre-existing fallback's raw-struct return already satisfies
            // the verifier for a concrete-typed default, so this case keeps
            // it.
            bool structuralNullableCarrier = false;
            if (auto* st = llvm::dyn_cast<llvm::StructType>(l->getType())) {
                structuralNullableCarrier =
                    st->getNumElements() == 2 &&
                    st->getElementType(0) == nullablePrimHasValTy();
                if (structuralNullableCarrier) {
                    const sema::TypeRef* rt =
                        bin.right ? ctx_.exprType(*bin.right) : nullptr;
                    bool rhsBareGenericT =
                        rt && rt->kind == sema::TypeRef::Kind::Named &&
                        !rt->declaration;
                    structuralNullableCarrier = rhsBareGenericT;
                }
            }
            if (isNullableValueTy(effectiveLt) || structuralNullableCarrier) {
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
                // fix #5017 (§7.7 typing + §5.1 widening, v0.307): a BINARY
                // FLOAT payload (`double? ?? -1`, `double? ?? f` with
                // `f: float`). §7.7 types the result `T` when the right
                // operand converts to `T`, and §5.1 widens int → float/double
                // and float → double — but neither arm above covers a float
                // payload, so the RHS reached the PHI at its own type and the
                // verifier aborted ("PHI node operands are not the same type
                // as the result!"). coerceNumericToType is §5.1's widening;
                // signedness is the RHS's own (uint → UIToFP).
                if (!rhsDiv && primVal->getType()->isFloatingPointTy() &&
                    r->getType() != primVal->getType() &&
                    (r->getType()->isIntegerTy() ||
                     r->getType()->isFloatingPointTy()))
                    r = coerceNumericToType(r, primVal->getType(),
                                            isUnsignedTargetExpr(*bin.right));
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
    // fix #1948 / §7.7 / §28.3 (producers arm-meet, v0.184; v0.220) / §28.9: a
    // null-coalescing `a ?? b` whose result is a (non-nullable) owned `string`.
    // §28.3's `a ?? b` producers row is the meet of a's and b's ownership:
    // "owned ⊓ owned = owned (exactly one of the two owned values is produced
    // at runtime and is discharged once — no leak, no double-free)". For a
    // runtime-managed `string` (§28.9) the result must carry EXACTLY +1 on
    // whichever arm the join takes. Normalize each arm to +1 IN ITS OWN BLOCK
    // before the endBB PHI — the SAME per-arm mint lowerCaseExpr/lowerConditional
    // apply to the sibling case/when result and the if-expression (#1934/#1948):
    //   • LHS arm (this entry block, the non-null path l→endBB): retained unless
    //     it is itself a +1 producer or a string local owning +1. The retain is
    //     emitted UNCONDITIONALLY here, but on the *null* path l is null and
    //     bz_string_retain is a null no-op — so it counts only when l is the
    //     result (the non-null path), exactly where the PHI takes l.
    //   • RHS arm (below, in rhsEndBB): retained unless a +1 producer / owning
    //     local, minting the +1 the null path needs.
    // The whole `??` is then a single +1 producer (isPlusOneStringProducer
    // recognizes it) the consumer TRANSFERS. Both halves are required:
    // recognition WITHOUT the per-arm mint drops a borrow arm's reference (UAF);
    // the mint WITHOUT recognition still double-retains a producer arm (the leak
    // — #1948).
    //
    // NON-NULLABLE ONLY (fix #1952 rework): the per-arm mint is confined to a
    // non-nullable `string` result. A `string?` `??` (both operands nullable) is
    // NOT normalized here — minting +1 for a borrow arm of a `string?` `??`
    // leaked when the result flowed through the un-refcounted nullable return
    // path (`return a ?? b` over `string?` borrows; #2196). The non-nullable
    // `??` result is discharged by the non-nullable consumers (return / bind),
    // so its borrow-arm mint is always balanced.
    const sema::TypeRef* coalResTy = ctx_.exprType(bin);
    bool coalNonNullString =
        coalResTy && !coalResTy->nullable &&
        coalResTy->kind == sema::TypeRef::Kind::Primitive &&
        coalResTy->primitive == sema::PrimitiveType::String;
    if (coalNonNullString && l->getType()->isPointerTy() && bin.left &&
        !isPlusOneStringProducer(bin.left.get()) &&
        !returnedStringLocalOwnsPlusOne(bin.left.get()))
        emitStringRetain(l);
    builder_.CreateCondBr(isNull, rhsBB, endBB);

    builder_.SetInsertPoint(rhsBB);
    llvm::Value* r = lowerExpr(*bin.right);
    bool rhsDiverged = diverged_;  // CG40: e.g. `a ?? throw E`
    if (!r) return nullptr;
    // fix #1948 (RHS arm of the string `??` normalization above): mint the +1
    // the null path's result needs, in rhsEndBB (the block r is defined in, which
    // flows to endBB), before the branch. Skipped when the RHS is itself a +1
    // producer / owning local (it already carries its single reference) or when
    // it diverged (never reaches endBB).
    if (coalNonNullString && !rhsDiverged && r->getType()->isPointerTy() &&
        bin.right && !isPlusOneStringProducer(bin.right.get()) &&
        !returnedStringLocalOwnsPlusOne(bin.right.get()))
        emitStringRetain(r);
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
                // fix #3143 (§14.1 / §2.5, v0.254): parameter names through the
                // one both-sources walk, so an IMPORTED generic record's unary
                // operator specializes too.
                const std::vector<std::string_view> gpNames =
                    sema::genericParamNames(opTy->declaration);
                if (!gpNames.empty() &&
                    gpNames.size() == opTy->genericArgs.size()) {
                    std::unordered_map<std::string_view, llvm::Type*> ns;
                    std::unordered_map<std::string_view,
                        const sema::TypeRef*> nsem;
                    for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                        ns[gpNames[gi]] =
                            lowerTypeRef(opTy->genericArgs[gi]);
                        nsem[gpNames[gi]] =
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
    if (!v) {
        // fix #2805 (§17.7 / §8.6, v0.240): `not X` where X failed to lower.
        // Returning null here propagates the failure OUTWARD, and the enclosing
        // short-circuit chain then degrades the whole `not X` to `false` — the
        // WRONG polarity. A dropped test reads as `false` (#2775), so `not` of
        // it must read as `true`; measured before this arm,
        // `not (o is [3]int) and oi is int` answered false where true is
        // required. Degrading the OPERAND and negating it keeps the polarity
        // correct and keeps the degrade in one place.
        //
        // Only `not` is degraded. Every other unary operator is arithmetic on a
        // value whose type we do not know here, so there is no safe stand-in —
        // those keep propagating null, which is their established contract.
        if (diverged_ || un.op != "not" || !un.operand) return nullptr;
        v = degradedBoolOperand({un.operand.get(), &un},
                                "operand of `not`");
    }
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

    // fix #5137 (§7.11, v0.313): the §7.11 'no three-valued logic' gate at
    // `not` (src/sema/ExprTypecheck_Exprs.cpp ~7959) admits a `bool?`
    // operand narrowed non-null by §7.4. Sema returns success and codegen
    // receives the operand as the declared `{i8 hasVal; i1 payload}` struct,
    // but `CreateNot` below needs an `i1` — extract the payload here. The
    // pre-extraction soundness precondition of unwrapNarrowedNullableOperand
    // (a sema gate that rejects the un-narrowed form) is satisfied by the
    // #5137 sema half in this same lane.
    if (op == "not") v = unwrapNarrowedNullableOperand(v, un.operand.get());

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
                // (d) fix #4016 (§28.3 K3.4/K7.4, v0.294): this bare `move x`
                // is itself the K3.4 marked-arm-transfer VALUE
                // (isBareMoveArmValue) — the ownership pass
                // (consumeMarkedArmTransfer / processMarkedArmConstruct,
                // Ownership_Checks.cpp) flagged `x`'s LocalDecl
                // `dischargedByArmTransfer` and registered a NULL-GUARDED
                // scope-end dynamicDispose for it (LlvmBackend_Stmts.cpp),
                // exactly the same per-path mechanism the `owned x`
                // (MarkedTransferExpr) spelling's OWN dedicated codegen arm
                // (LlvmBackend_Exprs.cpp, "arm.take") unconditionally nulls
                // for. That dedicated arm is never reached for THIS spelling
                // (a bare `move` is a plain UnaryExpr, not a
                // MarkedTransferExpr), so without this the selected path
                // read the slot but never nulled it: the not-selected-arm
                // fix (armIsOwnedProducer recognizing a bare `move` arm,
                // OwnershipFacts.cpp) made codegen correctly reclaim `x`'s
                // block via its own explicit/scope-end disposal, and this
                // slot's UN-nulled per-path scope-end dynamicDispose then
                // read + freed the SAME already-freed block a second time —
                // a heap-use-after-free, measured on this fix's own dev
                // cycle. Unconditional (no isUsing/isIterVarAutoDrop/
                // autoDropAtScopeEnd gate needed): consumeMarkedArmTransfer
                // only ever flags a plain, non-using class/interface LOCAL
                // (LOCALDECL-ONLY, its own comment), so this never overlaps
                // the other three cases.
                const bool isArmTransferDischarge = [&]() -> bool {
                    // LOCALDECL-ONLY: `dischargedByArmTransfer` is set ONLY by
                    // consumeMarkedArmTransfer (Ownership_Checks.cpp), which
                    // requires an Owned production a §21.1 `const` local can
                    // never bind, so the flag can never be set on a
                    // LocalConstDecl target. `loc->decl` here is always a var
                    // LocalDecl. Mirrors Ownership.cpp:991's identical
                    // reasoning for this SAME flag. // per §21.1, §28.3 K3.4, #3591
                    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl))
                        return ld->dischargedByArmTransfer;
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
                //
                // fix #4049 (§28.3/§28.8, v0.294): a plain-`var` LOCAL whose
                // OWN `autoDropAtScopeEnd` (LocalDecl's flag, set by
                // Ownership.cpp's nested-scope loop ~:1117) is armed — the
                // ownership pass's dataflow never runs inside lambda bodies, so
                // a `move x` reached inside a lambda body is invisible to
                // `everDischarged_` and the auto-drop is wrongly armed even
                // though `move x` actually transfers the value out. Nulling
                // here is the SAME null-site fix case (c) applies for the
                // out-owned-recipient twin — the scope-end disposal is
                // null-guarded (registerUsingDisposal's dynamicDispose, fired
                // by lowerStmt's emitAllDisposals), so nulling keeps the
                // dispose a no-op and prevents the lambda-body double-free
                // (the moved-out pointer otherwise reaches the caller AND the
                // lambda's scope-end dispose fires on the SAME slot — measured
                // heap-use-after-free under ASan). SAFETY: gated on
                // `everDischarged_.count(x) == 0` in the ownership pass's
                // nested-scope auto-drop loop (the loop above), so this null
                // never overlaps a method-body shape where the move is on a
                // conditional path and the slot must remain live for a
                // subsequent borrow — the plain-`var` borrow-after-move idiom
                // (`new T(move n, n)`) keeps its UN-nulled slot because n's
                // autoDropAtScopeEnd is false there. // per §28.3, §28.8
                // LOCALDECL-DEBT: #4049 — fix #4049 adds this null-site for the
                // plain-`var` local whose OWN autoDropAtScopeEnd is armed; the
                // flag lives ONLY on LocalDecl (not LocalConstDecl), so widening
                // the candidate vector to `const ast::Node*` would be a no-op
                // here (a §21.1 `const` local cannot bind the plain-`var` shape
                // — mirrors the LOCALDECL-ONLY reasoning above at line 600).
                const bool isPlainVarAutoDropAtScopeEnd = [&]() -> bool {
                    if (auto* ld =
                            dynamic_cast<const ast::LocalDecl*>(loc->decl))
                        return ld->autoDropAtScopeEnd;
                    return false;
                }();
                // (e) fix #4734 (§35.5 *The owning match*, §28.8.1,
                // v0.307): this bare `move` consumes an arm binding an OWNING
                // match transferred a payload into — §35.5's "owned local of
                // the arm body's scope". #4734 newly admits the section's
                // other three discharges on such a binding (`r.Dispose()`,
                // `_ := r;`, an owned-consuming call), each of which reclaims
                // through the binding's own slot under a null guard. The
                // ownership dataflow keeps NO `states_` entry for a pattern
                // binding, so it cannot reject a SECOND consume the way it
                // rejects `move x` twice on an ordinary owned local (§28.8.1:
                // "a moved-from binding is unreadable until reassigned") —
                // and without this null, `using var u := move w;
                // w.Dispose();` freed a block `u` also owns. Nulling here
                // makes the later reclaim reload null and skip, exactly the
                // trade case (d) records for the K3.4 arm transfer. Stamped
                // per-occurrence by the ownership pass, which only ever
                // stamps a binding it PROVED the owning match transfers, so a
                // BORROWING match's arm binding (whose `move` is #4664's hard
                // error) can never reach here. // per §35.5, §28.8.1, v0.307
                const bool isOwningMatchTransferConsume =
                    id->owningMatchTransferBinding &&
                    dynamic_cast<const ast::BindingPattern*>(loc->decl) !=
                        nullptr;
                // (f) fix #4794 (§28.8.1 / §28.3 / §29.1, stdlib §4.13,
                // v0.307): a plain local whose freshly produced
                // iterator-format source `registerIterableSourceDisposal`
                // claimed for a null-guarded scope-end slot-1 Dispose
                // (`var view := Gen();`, `var view := pool.Drain();`). The
                // `move` hands that frame to its new owner — an `owned`
                // parameter whose callee may dispose it — so the caller's
                // claim must not fire on it again. MEASURED pre-fix (ASan):
                // `var view := Gen(); var r := Take(move view);` with `Take`
                // calling `src.Dispose()` was a heap-use-after-free in the
                // caller's scope-end Dispose. Nulling makes that claim reload
                // null and skip — case (c)'s null-site reasoning; the claim
                // is exactly-once-safe under a conditional move for the same
                // reason. // per §28.8.1, §28.3, §29.1, v0.307
                const bool isClaimedIterableSource =
                    iterableSourceClaimedDecls_.count(loc->decl) != 0;
                if (loc->isUsing || isIterVarAutoDrop ||
                    loc->autoDropAtScopeEnd || isArmTransferDischarge ||
                    isPlainVarAutoDropAtScopeEnd ||
                    isOwningMatchTransferConsume ||
                    isClaimedIterableSource) {
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
                // move (lowerMoveFromCollectionSlot). Only pointer-typed owned
                // fields carry a disposal obligation: class/interface references
                // (isOwnedReferenceField), builtin iterator-interface frames
                // (isOwnedIteratorInterfaceField), AND owning-collection handles
                // (ownedCollectionFieldType — bz_array_t*/bz_llist_t*/bz_dict_t*).
                //
                // #2185 / §28.8.1 / §28.7: owning-collection fields MUST be in
                // this null-set. Otherwise the spec-canonical build-before-take /
                // restore-then-return shape (`using var t := move _arr; _arr :=
                // fresh;`) double-frees: the field keeps the moved-out pointer, so
                // the §28.7 drop-on-overwrite store (`lowerOwnedCollectionField
                // Store`) calls bz_array_free on the value the new owner (here the
                // using-var `t`) already owns — and `t`'s scope-end disposal then
                // frees the same block again (free()-on-a-bogus-header SEGV). The
                // auto-dispose path (emitOwnedFieldDisposals) and bz_array_free
                // are null-safe, so nulling here makes both the drop-on-overwrite
                // and any later disposal skip the moved-out collection cleanly.
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
                if ((isOwnedReferenceField(fs) ||
                     isOwnedIteratorInterfaceField(fs) ||
                     ownedCollectionFieldType(fs) != nullptr) &&
                    !emittingDisposerBody_) {
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
            // §28.8.1, #897 #898 (Part B): the QUALIFIED `move recv._field`
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
            // NOT inside a destructor body (emittingDisposerBody_): per the
            // #383/#763 design, a `move this._f` in a destructor does NOT get its
            // own scope-end disposal — emitDispose suppresses it — and the
            // SOLE disposal site is the owned-field auto-dispose pass
            // (emitOwnedFieldDisposals), which loads the field and disposes it.
            // Nulling the field here would make that pass skip the still-owned
            // object → a LEAK (destructor_field_move_runtime_763). The Part A
            // sema check already rejects a double-move in a destructor (so the
            // UAF this null guards against cannot occur there), so skipping the
            // null in the disposer body is both necessary and sound. This gate
            // is common to both the `this` and non-`this` receiver arms below.
            if (auto* fs = dynamic_cast<sema::FieldSymbol*>(
                    ctx_.exprSymbol(*ma));
                fs && (isOwnedReferenceField(fs) ||
                       isOwnedIteratorInterfaceField(fs) ||
                       ownedCollectionFieldType(fs) != nullptr) &&
                fs->parent && !emittingDisposerBody_) {
                auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    ma->target.get());
                // A NON-`this` receiver is nulled only when it is an OWNED place
                // with a stable, side-effect-free slot: an identifier resolving
                // to a local / using-var / `owned` param. fieldAddr re-lowers the
                // receiver to re-locate the field slot — an idempotent pointer
                // load for such an identifier. A receiver denoting a temporary
                // (`getBox().f`) has no stable slot and re-lowering would re-run
                // its side effects, so it is NOT handled here (a separate case,
                // #2002 deferred). Ownership is guaranteed by sema: a borrowed
                // receiver's owned field cannot be moved out at all (rejected
                // before codegen — #1999), so any `move recv.f` reaching here is
                // on a receiver this scope owns.
                bool nonThisStableOwned = false;
                if (tid && tid->name != "this") {
                    sema::Symbol* rsym = ctx_.exprSymbol(*tid);
                    nonThisStableOwned =
                        dynamic_cast<sema::LocalSymbol*>(rsym) != nullptr ||
                        dynamic_cast<sema::ParameterSymbol*>(rsym) != nullptr;
                }
                if (tid && tid->name == "this" && currentThis_) {
                    // §28.8.1: the `this._field` fast-path. Compute the slot via
                    // currentThis_ + the field index — the same lightweight
                    // StructGEP implicitFieldAddr uses for the bare form — NOT
                    // fieldAddr, whose re-lowering of the `this` receiver and
                    // generic-substitution side effects produced malformed IR
                    // for the canonical destructor `using var u := move
                    // this._inner;` case (it re-emitted the receiver after the
                    // field value `v` had already been read).
                    auto idxIt = fieldIndex_.find(fs);
                    if (idxIt != fieldIndex_.end()) {
                        llvm::StructType* st = classStruct(fs->parent);
                        if (st && st->getElementType(idxIt->second)
                                      ->isPointerTy()) {
                            llvm::Value* addr = builder_.CreateStructGEP(
                                st, currentThis_, idxIt->second);
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), addr);
                        }
                    }
                } else if (nonThisStableOwned) {
                    // #2002 / §28.7 / §28.8/§28.8.1 (v0.197): the QUALIFIED move
                    // out of a NON-`this` owned receiver's field. Before this,
                    // ONLY `this`-field moves nulled their slot, so even the
                    // otherwise-sound "build then take then restore" form —
                    // `var t := move box.f!; box.f := null; C.Sink(move t!)` —
                    // double-freed: `box.f` kept the moved-out pointer, so the
                    // §28.7 drop-on-overwrite `box.f := null` disposed the value
                    // `t` already owns (and a scope-end disposal of `box` would
                    // do the same). Null the source field slot so a subsequent
                    // read / `:= null` / owner disposal sees null and skips.
                    //
                    // Use fieldAddr (not a hand-rolled StructGEP on the lowered
                    // receiver pointer): it installs the receiver's generic
                    // substitution from ma->target's exprType, so a field of a
                    // DIFFERENTLY instantiated generic receiver (`Box<int>` vs
                    // the enclosing method's `T`) GEPs the correct specialized
                    // layout. A raw StructGEP under the enclosing method's
                    // (possibly different) substitution could compute a wrong
                    // byte offset and corrupt memory. Re-lowering the identifier
                    // receiver inside fieldAddr is an idempotent pointer load.
                    llvm::Type* fty = nullptr;
                    if (llvm::Value* addr = fieldAddr(*ma, fty)) {
                        if (fty && fty->isPointerTy())
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), addr);
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

// fix #4210 (§27.5 / §28.9, v0.298): the module-level global that stores
// the most recently freed cell pointer. The envdtor stores the freed cell
// here before calling free(). boxLocalSlot checks this global; if the
// cached cell matches the stored freed pointer, the entry is stale and
// fresh allocation runs.
llvm::GlobalVariable* Emitter::getFreedSlotGlobal() {
    auto* gv = module_->getGlobalVariable("bz_freed_slot");
    if (!gv) {
        gv = new llvm::GlobalVariable(
            *module_, ptrTy(), /*isConstant=*/false,
            llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantPointerNull::get(ptrTy()),
            "bz_freed_slot");
    }
    return gv;
}


llvm::Value* Emitter::lowerLambda(const ast::LambdaExpr& lam) {
    const std::vector<sema::Symbol*>* paramSyms = ctx_.lambdaParams(&lam);
    // fix #4813 (§14.1 / §14.5, v0.307): snapshot the hint map BEFORE this
    // lambda's own parameter hints are set below, so the restore on exit drops
    // them. A hint is resolved through the ACTIVE specialization's
    // substitution; taken after the loop, the snapshot kept it, and the next
    // specialization of the same lambda AST found the stale hint (the
    // find-then-insert below, or a path that sets none — an `int` binding):
    // `Go<bool>(true)` then `Go<int>(5)` printed `<True>` twice, and a
    // string/double hint on the int specialization failed the IR verifier.
    auto savedLambdaParamHints = lambdaParamTypeHint_;  // restore after body

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
                // fix #3801 (§27.3 / §27.4, v0.287): also backfill
                // lambdaParamTypeHint_ from the inferred resolvedDeclType so
                // body identifier references (e.g. `b.Length` on a string-typed
                // inferred param, or `a(7)` on a function-typed inferred param)
                // see the correct sema type via the lambdaParamTypeHint_ lookup
                // sites in LlvmBackend_Exprs.cpp:1140/:1927/:6192. The earlier
                // expectedFunctionType_-gated SETs (below) only fired when an
                // expected function type was threaded in — the IIFE path in
                // LlvmBackend_Calls2.cpp:5421 calls lowerLambda directly
                // without setting expectedFunctionType_, so for an IIFE param
                // whose sema-resolved type is non-i32 (function types, string,
                // class types — anything that lowerTypeRef maps to a non-i32
                // LLVM type) the hint was never set, and `r=0` came out
                // silently. Use find-then-insert so we don't clobber the
                // genericSubst_-resolved hint the expected-function-type path
                // may have already set above for an overlapping param.
                if (!par->declType &&
                    lambdaParamTypeHint_.find(par) == lambdaParamTypeHint_.end())
                    lambdaParamTypeHint_[par] = par->resolvedDeclType;
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
                    // fix #4896 (§18.5 interpolated strings / §14.5 phase
                    // 2, v0.307): when
                    // the formal param type is itself an unresolved
                    // type-parameter reference (Named, no declaration,
                    // single-part name) and genericSubstSemaTy_ has a
                    // CONCRETE substitution recorded for that same name
                    // (the Calls2.cpp pre-pass's char/enum i32-collision
                    // workaround — see the `lt == i32()` arm there), use
                    // that concrete TypeRef as the hint. Without this, an
                    // inferred lambda param specialized at `char` or an
                    // `enum` keeps the bare formal TypeRef, so `y.ToString()`
                    // inside `$"<{y}>"` dispatches on the int path (§17.5
                    // hole dispatch uses the receiver's static type) and
                    // prints the code point / ordinal instead of the
                    // character / enum name.
                    //
                    // genericSubstSemaTy_ can ALSO hold a still-bare entry
                    // (the non-i32 pre-pass branch records the unsubstituted
                    // TypeRef verbatim when it is nested inside another
                    // generic method's own monomorphization — e.g. `W → T`
                    // while Go<T>'s own `T` is resolved elsewhere; this
                    // pre-pass substitution has been in place since #4813
                    // (bb41c32f), not merely since #4896 as an earlier
                    // revision of this comment said — #5024) — using THAT as
                    // a hint would regress the
                    // pre-existing, working path for those types (measured:
                    // a nested `Go<string>` call printed a raw pointer
                    // value instead of the string once this arm stopped
                    // gating on bareness). Require `!isBareTypeParamRef` on
                    // the STORED entry too, so this arm only fires when the
                    // stored substitution is genuinely concrete.
                    //
                    // Skip a NULLABLE substitution (`int?`, `Color?`, …):
                    // the interpolation dispatch already special-cases a
                    // null receiver as the empty string without calling
                    // `T?.ToString()`; injecting a nullable hint here would
                    // route the lambda body through the non-nullable
                    // primitive dispatch, mismatching the caller's actual
                    // (nullable-wrapped) value and risking an IR
                    // verification failure. Leaving the hint unset here
                    // preserves the pre-existing nullable-path behavior.
                    else if (expRT->kind == sema::TypeRef::Kind::Named &&
                             !expRT->declaration && expRT->astNode &&
                             expRT->astNode->name &&
                             expRT->astNode->name->parts.size() == 1 &&
                             !genericSubstSemaTy_.empty()) {
                        auto sit = genericSubstSemaTy_.find(
                            expRT->headName());
                        if (sit != genericSubstSemaTy_.end() &&
                            sit->second && !sit->second->nullable &&
                            !isBareTypeParamRef(sit->second)) {
                            lambdaParamTypeHint_[par] =
                                const_cast<sema::TypeRef*>(sit->second);
                        }
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
            // fix #3095 / §4.7 (v0.252): a `()`-typed lambda parameter lowers to
            // LLVM `void`, which is not a valid parameter type — the store of
            // the incoming argument into its slot below traps
            // (`DataLayout::getABITypeAlign(void)`, SIGILL, measured on the
            // #3095 fix's own base). Substitute the one-byte placeholder, the
            // same substitution `lowerParamType` makes for a named method's
            // parameter and `lowerLocalDecl` (#1619) makes for a unit local.
            // Reached however `ty` was derived — the written annotation, the
            // inferred `resolvedDeclType`, or the expected function type — so no
            // derivation path can reintroduce the trap. `()?` never needs it: it
            // is the tagged `{i8, i8}` optional (isNullableUnitTy). // per §4.7
            //
            // fix #3527 follow-up: asked through `unitStorageTy` — the one
            // spelling of this substitution — rather than repeated inline. The
            // expression is character-for-character what stood here before.
            ty = unitStorageTy(ty);
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
    if (lam.retType) {
        // §27.3 (v0.217, #2101): a return-type ASCRIPTION is the declared
        // return type — sema stored it as the lambda's funcReturn, and it is
        // authoritative over the body-derived type for BOTH body kinds (an
        // expression body of a narrower/nullable-underlying type is coerced
        // by the emit paths below, exactly as the expectedFunctionType_
        // override case). A unit ascription lowers to void (the §27.3
        // discarded-result procedure form).
        if (sema::TypeRef* lamTy = ctx_.exprType(lam)) {
            if (lamTy->kind == sema::TypeRef::Kind::Function &&
                lamTy->funcReturn) {
                retTy = lowerTypeRef(lamTy->funcReturn);
                retSemaTy = lamTy->funcReturn;
            }
        }
    } else if (lam.exprBody && lam.isAsync) {
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
            // exprType from sema (ExprTypecheck_Calls.cpp #549-553); lowerTypeRef
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
    //
    // fix #5041 (§14.1 / §14.5, v0.307): the formal's return may be a bare
    // method type parameter (`f: (T) -> U` at U := int?). Resolve it through
    // the active substitution first, so the concrete `int?` both fixes the
    // declared return type AND becomes retSemaTy — the expr-body emit wraps
    // the body value into the `{i8, i32}` carrier only when retSemaTy is the
    // nullable value type. Otherwise `lambda(y) => y + 1` returned a bare i32
    // from a `{i8, i32}` function (IR verifier) and `lambda(y) => null`
    // returned `ptr` while the specialization expected the carrier.
    //
    // Only the CALLEE's own method type parameters are resolved this way, and
    // only for a lambda bound to one of that callee's formals (per §14.1 a
    // type parameter belongs to its declaration): genericSubstSemaTy_ also
    // holds the ENCLOSING generic method's bindings, so a by-name lookup took
    // `Box<T>`'s `T` in `Box<string>.Apply(v, f: (T) -> T)` for an enclosing
    // `Go<T>`'s `T := int?` and declared the lambda `{i8, i32}` over a
    // `string` body. The pre-pass records that set (lambdaCalleeOwnTParams_).
    const sema::TypeRef* expectedRetSema =
        expectedFunctionType_ ? expectedFunctionType_->funcReturn : nullptr;
    if (expectedRetSema && !genericSubstSemaTy_.empty() &&
        lambdaCalleeOwnParams_ && isBareTypeParamRef(expectedRetSema) &&
        lambdaCalleeOwnTParams_.count(expectedRetSema->headName())) {
        bool isCalleeFormal = false;
        for (const auto& p : *lambdaCalleeOwnParams_)
            if (p.type && ctx_.resolvedType(*p.type) == expectedFunctionType_) {
                isCalleeFormal = true;
                break;
            }
        if (isCalleeFormal)
            expectedRetSema =
                substituteTypeRefLocal(expectedRetSema, genericSubstSemaTy_);
    }
    if (expectedRetSema && isNullableValueTy(expectedRetSema)) {
        llvm::Type* expected = lowerTypeRef(expectedRetSema);
        // #5041: the i32-fallback override above may already have set retTy
        // to the carrier (lowerTypeRef(U) through genericSubst_) while
        // leaving retSemaTy the bare `U`; refresh retSemaTy in that case too.
        if (expected != retTy || expectedRetSema != expectedFunctionType_->funcReturn) {
            retTy = expected;
            retSemaTy = expectedRetSema;
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
        // fix #2706 / §27.5 / §28.9 (v0.240): true when this capture's cell is
        // WRITTEN by the enclosing function (or by a lambda body) after it can
        // be captured, AND more than one env can share that one cell. The
        // per-capture retain below and the envdtor release then refer to
        // DIFFERENT strings — the retain lands on the value present at capture,
        // the release on whatever the last write left in the cell — so N envs
        // release the final value once each against its single +1: a double
        // free. Both sides are suppressed together (the retain is skipped where
        // this is set, the envdtor's release is skipped on the same flag), which
        // leaks the cell's final value instead of over-releasing it. Never set
        // for a box capture (releaseBoxInEnvdtor is a disjoint branch on a
        // non-String, non-Function sema type).
        //
        // RESIDUAL, tracked as #2718: the leak this trades for. The sound model
        // refcounts the CELL (rc(contents) must equal the LIVE ENV COUNT, a
        // runtime quantity), which is a design change, not a local patch — four
        // local patches were tried and each is unsound, see #2718. Write paths
        // this gate does not see are #2719.
        bool                   suppressContentRcInEnvdtor = false;
        // fix #4509 (§27.5 / §28.9, v0.306): this capture's cell is REFCOUNTED
        // — the env announces itself with `bz_cell_retain` at the capture-store
        // and drops that reference with `bz_cell_release` in the envdtor, and
        // the declaring scope's end drops the frame's implicit one (a
        // `Disposal::cellRelease` entry in the #1017 SECOND PASS). Set only for
        // a cell that every STATIC route declines — an escaping capturer exists
        // (`declsCapturedByEscaping_`) so `cellFreeEligible_` refused the
        // scope-end free, and neither #694's nor #3426's envdtor route armed —
        // i.e. exactly the cells that leak today. Because those cells have NO
        // free emitted for them anywhere, adding a balanced retain/release pair
        // cannot introduce a double free; the worst arithmetic error is a
        // surviving leak. Disjoint from `freeCellInEnvdtor` by construction.
        bool                   retainCellInEnv = false;
    };
    std::vector<CaptureEntry> captures;

    // Save state (needed for capture slot access and restoration).
    // (The hint map is snapshotted at function entry — fix #4813.)
    // fix #4813 (§14.1 / §14.5, v0.307): entries recorded past this mark
    // are this lambda's hint-injected expression types; cleared on exit below.
    const std::size_t hintInjectedMark = lambdaHintInjectedExprs_.size();
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
    // fix #2886 (§23.3/§27.3/§28.3, v0.240): a lambda emitted inside an
    // INLINED block-bodied property setter must not inherit
    // `inlinedAccessorEnd_` — that block belongs to the enclosing (caller)
    // function, and a bare `return;` inside the lambda body would otherwise
    // branch to it (`LlvmBackend_Stmts.cpp` ~1719), a cross-function `br`
    // the LLVM verifier rejects as `Referring to a basic block in another
    // function!`. Mirror the `unwindTarget_` save/clear/restore pattern: save
    // the enclosing body's value, null it across lambda emission, restore on
    // exit. (A bare `return;` inside the lambda body now falls through to
    // the ordinary void-return path the lambda's own function emits.)
    llvm::BasicBlock* savedInlinedAccessorEnd = inlinedAccessorEnd_;
    // #2515 / §27.3 / §28.9: save the enclosing method's return-type node and
    // reset it for the lambda body (see the nullptr assignment at body-entry
    // below).  Without this, a block-bodied lambda compiled inside a method
    // whose declared return type is a FUNCTION type (e.g. `() -> Animal`) leaks
    // that node into the body's ReturnStmt lowering: emitReturn's
    // closure-return-retain branch (LlvmBackend_Stmts.cpp ~line 1828) sees a
    // Function declRet and fires emitClosureRetain on a NON-closure return
    // value (a captured class instance), writing 8 bytes at offset 16 of a
    // 16-byte object — a heap-buffer-overflow that surfaces as a double-free
    // under ASan.  The expr-body path is unaffected (it keys off retSemaTy, the
    // lambda's OWN return type); only the block-body path goes through
    // emitReturn, which reads currentReturnTypeNode_.
    const ast::Type* savedCurrentReturnTypeNode = currentReturnTypeNode_;
    // fix #2817 / #2807 / #429 / §4.6 / §28.9 / §28.3 (v0.240): save the `var`
    // STRING-local refcount state across the WHOLE lambda emission. A lambda body
    // is lowered MID-EMISSION of its enclosing function, and both the three sets
    // and their fn token are per-function state owned by that enclosing body — so
    // the pin below (and the one emitAsyncLambda performs for an async lambda's
    // resume fn) must not survive back into it. This is the same discipline #1483
    // established for the unwind sets and #2807 had to add to
    // buildSpecializedMethodFn: without the restore, every string local the
    // enclosing body declares AFTER this `var f := <lambda>` silently loses its
    // bind-time retain and scope-end release (the leak direction, but a
    // regression this change would have introduced — #2807 measured exactly that
    // for the specialization case).
    auto savedStringReleaseEligible = stringReleaseEligible_;
    auto savedStringBorrowRetainEligible = stringBorrowRetainEligible_;
    auto savedStringBorrowAll = stringBorrowAll_;
    auto savedStringLocalCandidates = stringLocalCandidates_;
    auto savedValueLikeChoiceStrReleaseEligible =
        valueLikeChoiceStrReleaseEligible_;
    llvm::Function* savedStringReleaseEligibleFn = stringReleaseEligibleFn_;
    // fix #2641: the analyzed-body pointer travels with the eligibility sets
    // above — a nested lambda's own collectStringReleaseEligible repoints it, and
    // without this restore the enclosing body's later inline `out var` recipients
    // would be escape-walked against the LAMBDA's statements (the wrong body:
    // an escaping use would go unseen ⇒ a scope-end release of a value the
    // enclosing body still aliases). Same failure shape #2807 measured for the
    // sets themselves, in the UAF direction rather than the leak direction.
    const std::vector<std::unique_ptr<ast::Stmt>>* savedStringReleaseBody =
        stringReleaseAnalyzedBody_;
    // fix #4044 (§22.4 / §28.9 / §30.1 / §30.6, v0.295): same pattern as the
    // body pin above, but for an EXPRESSION-bodied async lambda's expr pin —
    // emitAsyncLambda's `lam.exprBody` arm sets
    // `stringReleaseAnalyzedExpr_ = lam.exprBody.get()`, and without this
    // restore the enclosing body's later inline `out var` recipients would be
    // escape-walked against the LAMBDA's expression body (the wrong scope).
    const ast::Expr* savedStringReleaseAnalyzedExpr = stringReleaseAnalyzedExpr_;

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
        // fix #3466 (§27.5 / §30.1, v0.283): the move above only ever fires when
        // the ORIGINAL slot already had a frame field, which for a PARAMETER it
        // never does — a parameter's cross-suspension restore is the resume
        // prologue's unconditional copy from its capture field into the
        // PRE-BOXING alloca, and that copy knows nothing about the cell holder
        // this capture just rebound paramSlots_ to. So the holder was a bare
        // entry-block alloca of the resume fn, re-nulled on every activation:
        // after a real suspension the body read back null — SIGSEGV on a read of
        // the parameter after the `await`, and a silent leak at the scope-end
        // cell free (which then freed null) where there was no such read.
        // buildStateMachineFrame reserves a dedicated ptr field per captured
        // parameter; wire the holder to it so the per-suspend spill/restore
        // (emitAwaitSuspend, the yield handler — both iterate smAllocaToField_)
        // carries it. Guarded on `!count(newSlot)` so a SECOND lambda capturing
        // the same parameter (which reaches boxLocalSlot's already-boxed path
        // with newSlot == slot) cannot re-key an existing mapping.
        if (currentFn_ == smCellFrameFn_ &&
            !smAllocaToField_.count(newSlot)) {
            auto pcit = smParamCellField_.find(sym);
            if (pcit != smParamCellField_.end())
                smAllocaToField_[newSlot] = pcit->second;
        }
        ce.sym = sym;
        ce.decl = nullptr;
        // #429: record freshly-boxed param cells that are eligible for free
        // at scope exit (cellFreeEligible_ pre-scan says the param is captured
        // only by non-escaping lambdas; params are never for-var).
        // Skip guarded-boxing cells — a param declared outside a loop whose
        // lambda capture is inside the loop body must not have its cell freed
        // at inner-scope exit (the cell is reused across iterations).
        //
        // fix #3403 (§27.5 / §28.9, v0.283): `loopStack_.empty()` is admitted
        // alongside `!wasGuardedParam`, which is #3435's captured-LOCAL proof
        // applied to the branch it was not applied to. The proof is about a
        // BACKWARD EDGE, not about the storage class: with no enclosing loop
        // above the capture site, boxLocalSlot's null-guarded allocation runs at
        // most once per invocation and exactly one closure holds the cell, so
        // the scope-end free is that cell's one and only free.
        //
        // For a PARAMETER the guarded path is not an exotic case, which is why
        // the old flat exclusion mattered so much more here than it did for a
        // local. boxLocalSlot picks fresh-vs-guarded by asking whether a
        // StoreInst to the slot sits in the CURRENT basic block; a parameter's
        // store is the prologue's, so the fresh path is taken only while the
        // capture site is still in the entry block. Any block-splitting
        // construct above it — an `if`, a `while`, a `case` — takes the guarded
        // path, and an ASYNC body takes it ALWAYS (the resume function opens
        // with the state-dispatch `switch`, so no body statement is ever in the
        // entry block). That is the whole of the issue's "leaks only after an
        // `await`" discriminator: it is the block boundary, not the await.
        if (!alreadyBoxed && (!wasGuardedParam || loopStack_.empty()) &&
            currentFn_ == cellFreeEligibleFn_ &&
            cellFreeEligible_.count(static_cast<const void*>(sym))) {
            LastCellEntry lce{newSlot, static_cast<const void*>(sym)};
            // fix #2334: propagate the TaskScope.Spawn unwind-unsafety onto
            // this entry so consumeLastLambdaCapturedCells suppresses the
            // free on the unwind edge (Disposal::skipCellFreeOnUnwind).
            lce.unwindUnsafe =
                unwindUnsafeCellFree_.count(static_cast<const void*>(sym)) !=
                0;
            // fix #3403: mark this a PARAMETER entry so the #969 nested-scope
            // guard in consumeLastLambdaCapturedCells applies to it. That guard
            // reads declScopeDepth_, which holds LOCALS only — so before this
            // flag existed a parameter captured inside a `do … end` had its cell
            // freed at the INNER scope's end and every later read of the
            // parameter (which goes through the cell after boxing) dereferenced
            // a nulled holder: a SIGSEGV, measured on the pre-fix compiler.
            lce.isParam = true;
            lce.pushScopeDepth = disposeStack_.size();  // fix #4261 §28.3
            lastLambdaCapturedCells_.push_back(lce);
        }
        // #429/#672: resolve sema type for retain/release classification.
        if (auto* par = dynamic_cast<const sema::ParameterSymbol*>(sym)) {
            if (par->resolvedDeclType)
                ce.semaTy = par->resolvedDeclType;
            else if (par->declType)
                ce.semaTy = ctx_.resolvedType(*par->declType);
        }
        // fix #2689 / §28.1 / §28.9 / §27.5: a captured PARAMETER of
        // runtime-managed type takes a retain at EVERY capture — where a
        // captured LOCAL takes one only on the 2nd-and-later capture (the
        // `alreadyBoxed` branch in the captured-locals loop below).
        //
        // The asymmetry is not an oversight there; it is the difference between
        // the two storage classes. A captured local's slot holds a reference
        // this frame owns, and escape detection SUPPRESSES its scope-end
        // release, so the first env inherits that suppressed release and only
        // additional envs must mint. A PARAMETER of `string` or function type
        // has no such reference to inherit: §28.1 makes the `owned` marker a
        // compile error on exactly these types — "On any other type — a
        // primitive, `string`, a function type, an ordinary record, an enum, a
        // non-owning inline array, or the unit type — `owned` is a compile
        // error: such values carry no disposal obligation to transfer (§28.9)"
        // — and erases it at a runtime-managed substitution of a bare type
        // parameter. So EVERY such parameter is an unmodified BORROW: the
        // caller owns the reference and releases it, and this frame has no
        // scope-end release for an env to take over.
        //
        // The envdtor nevertheless releases the cell's contents unconditionally
        // on the sema type (see its emission below). Without a retain here the
        // VERY FIRST capture therefore over-releases the caller's value — a
        // use-after-free, not a leak, and reproducible from one lambda over one
        // borrowed `string` parameter (#2689). N captures ⇒ N retains ⇒ N
        // envdtor releases, leaving the caller's +1 untouched.
        //
        // Emitted per capture-site EXECUTION, which is what also keeps a
        // capture site inside a LOOP balanced: a parameter always takes
        // boxLocalSlot's GUARDED path (its slot is written in the prologue, not
        // in the capture site's block), so ONE cell is reused across
        // iterations — but each iteration still builds its own closure with its
        // own envdtor run, so one retain per iteration pairs with one release
        // per closure. Both helpers are null-safe, so a null cell content is a
        // no-op. Nullable `string?` is excluded by isStringTypeRefNonNull on
        // both sides, matching the envdtor exactly — agreement between the
        // retain and release predicates is the invariant. A captured ref/out
        // param is a different loop below and stays at semaTy = nullptr: it
        // borrows the caller's storage and the envdtor releases nothing.
        //
        // fix #2706: EXCEPT when the body WRITES this parameter's cell and more
        // than one env can share it — see armMutatedSharedCaptureCell. The
        // retain and the envdtor release are then suppressed TOGETHER; keeping
        // the retain alone would merely move the leak.
        //
        // fix #4198: OR when the parameter is directly RETURNED BY NAME from
        // this function — see capturedLocalReturnedByName's declaration for
        // why that is a second, independent hazard (the return's silent
        // transfer and the envdtor's content release both claim the same
        // reference).
        ce.suppressContentRcInEnvdtor =
            mutatedSharedCaptureCell(
                static_cast<const void*>(sym), ce.semaTy,
                /*freshCellPerIteration=*/false) ||
            capturedLocalReturnedByName(static_cast<const void*>(sym),
                                         ce.semaTy);
        if (!ce.suppressContentRcInEnvdtor) {
            if (isStringTypeRefNonNull(ce.semaTy)) {
                llvm::Value* str =
                    builder_.CreateLoad(ptrTy(), ce.envValue, "str.pcap");
                emitStringRetain(str);
            } else if (ce.semaTy &&
                       ce.semaTy->kind == sema::TypeRef::Kind::Function) {
                llvm::Value* cl =
                    builder_.CreateLoad(ptrTy(), ce.envValue, "cl.pcap");
                emitClosureRetain(cl);
            }
        }
        // §27.5 / §28.9 / #694: free the capture CELL in the envdtor when this
        // PARAMETER is captured by exactly ONE env (singleCaptureLocals_, which
        // keys parameters by Symbol*) and THIS lambda DEFINITELY escapes
        // (definitelyEscapingLambdas_). Exactly the guard the captured-LOCALS
        // branch below applies, and sound for the same two reasons: the sole
        // holder means this envdtor is the cell's only free (no double-free), and
        // a definitely-escaping closure outlives its defining scope, so the frame
        // is gone before the envdtor runs (no UAF). The scope-end boxedCellFree
        // cannot also fire — collectCellFreeEligible excludes anything captured by
        // an escaping lambda, and a returned lambda is always classified escaping
        // there. Params are never for-var.
        //
        // GUARDED BOXING. A parameter's slot is written in the prologue, never in
        // the capture site's own basic block, so boxLocalSlot always takes the
        // GUARDED path for it (the null-guarded holder that allocates the cell on
        // the first pass and reuses it on later ones). The hazard guarded boxing
        // protects against is a capture site INSIDE A LOOP: one cell shared by
        // every iteration's closure, so any one envdtor freeing it would
        // double-free/UAF the others — and singleCaptureLocals_ cannot see that,
        // since it counts syntactic lambda NODES, not the closures a loop makes
        // from one node. `loopStack_.empty()` is the proof that this capture site
        // has no backward edge above it: the cell is then allocated exactly once
        // per invocation and held by exactly one closure. (The same
        // guarded-cell-allocated-once proof the captured-LOCALS scope-end path
        // below uses for an inline owned array.) A capture inside a loop keeps the
        // conservative exclusion — a bounded leak, never an over-free.
        if (!alreadyBoxed && (!wasGuardedParam || loopStack_.empty()) &&
            currentFn_ == boxCaptureLocalsFn_ &&
            isSharedCellEnvdtorFreeOwner(static_cast<const void*>(sym), &lam)) {
            ce.freeCellInEnvdtor = true;
        }
        // fix #4261-FORWARD (§28.3 / §27.5 / §28.9, v0.298): the SOLE-
        // ESCAPING-CAPTURE path for parameters. isSharedCellEnvdtorFreeOwner above
        // handles SHARED cells (captured by both escaping AND non-escaping lambdas);
        // it is FALSE for SOLE captures (only one lambda, the escaping one, captures
        // the parameter). For a sole escaping capture, sharedCellEnvdtorFreeOwner_
        // is never populated (capturedByNonEscaping is empty), so a parameter like
        // `n` in `return lambda() => n` never gets ce.freeCellInEnvdtor armed and
        // its cell leaks. Add the same two guards the captured-LOCALS path uses:
        // singleCaptureLocals_ (sole holder, no double-free risk) and
        // definitelyEscapingLambdas_ (the env outlives the frame, so the envdtor
        // is the cell's only free and is safe from UAF). Params are never for-var.
        if (!ce.freeCellInEnvdtor &&
            (!wasGuardedParam || loopStack_.empty()) &&
            currentFn_ == boxCaptureLocalsFn_ &&
            definitelyEscapingLambdas_.count(static_cast<const void*>(&lam)) &&
            singleCaptureLocals_.count(static_cast<const void*>(sym))) {
            ce.freeCellInEnvdtor = true;
        }
        // fix #3426 (§27.5 / §28.9, v0.283): the SHARED-cell twin of the branch
        // above. `singleCaptureLocals_` requires this env to be the cell's only
        // holder; when a non-escaping sibling lambda also captures the parameter
        // that fails, the scope-end route is disarmed too (the parameter is in
        // capturedByEscaping), and the cell was reclaimed by nobody.
        // sharedCellEnvdtorFreeOwner_ names THIS lambda the sole holder that
        // outlives the frame, which makes its envdtor the last reader — see the
        // ordering argument at the field's declaration. `alreadyBoxed` is NOT
        // required here: in the shared case the cell is typically created by the
        // FIRST (non-escaping) capture, so the escaping owner reaches
        // boxLocalSlot's already-boxed path. `loopStack_.empty()` replaces the
        // guarded-boxing exclusion as the backward-edge proof (that path reports
        // wasGuardedParam=false for an already-boxed slot, so it proves nothing
        // here).
        if (!ce.freeCellInEnvdtor &&
            currentFn_ == cellFreeEligibleFn_ &&
            isSharedCellEnvdtorFreeOwner(static_cast<const void*>(sym), &lam)) {
            ce.freeCellInEnvdtor = true;
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
    // Box and collect captured outer locals. fix #3521 (§31.1 / §31.2.1,
    // v0.283): carry the resolved LocalSymbol beside the decl node — this loop
    // already resolves it, and the capture body below needs the symbol's
    // `isCatchBinding` classification (it used to consult a codegen-local
    // `catchArmBindingDecls_` side set recorded by lowerTry, the third of
    // #3419's three spellings of the same fact).
    std::vector<std::pair<const ast::Node*, const sema::LocalSymbol*>>
        capturedLocalDecls;
    for (auto& [decl, slot] : locals_) {
        (void)slot;
        for (const sema::Symbol* s : capturedSyms) {
            if (auto* loc = dynamic_cast<const sema::LocalSymbol*>(s)) {
                if (loc->decl == decl) {
                    capturedLocalDecls.push_back({decl, loc});
                    break;
                }
            }
        }
    }
    for (const auto& [decl, declSym] : capturedLocalDecls) {
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
        bool isPatternBinding =
            dynamic_cast<const ast::BindingPattern*>(decl) != nullptr;
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
        // A case-arm binding's source slot is written by pattern matching each
        // time the arm is entered. Guarded boxing intentionally reuses one
        // holder, so refresh its cell from the newly-bound slot on every arm
        // entry; otherwise a case inside a loop would capture the first
        // iteration's payload forever. The holder remains live until the arm's
        // owner scope (or the escaping closure) releases it.
        //
        // fix #3503 (§31.2 / §27.5 / §28.9, v0.283): a CATCH-ARM binding is the
        // same shape and was NOT covered — `arm.body` is an ordinary Stmt, so
        // the `BindingPattern` test above cannot see it. Since fix #3521 the
        // fact is read off the binding's own symbol (`isCatchBinding`, set by
        // sema where it anchors the binding) rather than a lowering-time set.
        // Measured on `ef339fa`, a `try/catch` inside a
        // `while` whose arm captures the binding: heap-use-after-free in the
        // closure on iteration 2, because the guarded cell still held iteration
        // 1's exception object, which the arm's own disposal had already
        // reclaimed. That is PRE-EXISTING and reproduces on a purely
        // SYNCHRONOUS method; it becomes load-bearing here because the fix
        // above makes the holder survive a resume, which would otherwise have
        // turned the async spelling's SIGSEGV into exactly that use-after-free.
        //
        // Only the REFRESH is widened. The free-side admission (`guardedOk`
        // below, which lets a case-arm binding's guarded cell be reclaimed at
        // the arm scope even inside a loop) is deliberately NOT widened to catch
        // arms: that is a release-direction change and needs its own evidence.
        // The cost of leaving it is one leaked cell per method invocation, which
        // is the behaviour every guarded in-loop capture already has (#3403).
        if ((isPatternBinding || (declSym && declSym->isCatchBinding)) &&
            wasGuardedLocal) {
            llvm::Value* current = builder_.CreateLoad(ce.valTy, slot,
                                                       "pattern.capture.value");
            builder_.CreateStore(current, ce.envValue);
        }
        locals_[decl] = newSlot;  // rebind to the ptr cell-holder
        rebindDisposalSlot(slot, newSlot);
        // fix #3503 (§27.5 / §30.9 / §31.2 / §30.1, v0.283): a CATCH-ARM
        // BINDING has no bodyLocals frame field for the move below to relocate —
        // collectSuspendLocals never sees it, because lowerTry creates its slot
        // directly. Its cross-suspension storage is the handler SPILL AREA
        // instead: lowerTry registers the binding's own alloca in
        // asyncHandlerSpillSlots_ when the arm contains an `await` (§31.1 — the
        // borrow's lifetime is the whole arm body, which spans the suspend), and
        // promoteDemotedSlotsToFrame moves it into the frame's calloc'd spill
        // area. The cell HOLDER this capture just created was in NEITHER place:
        // a bare entry-block alloca of the resume fn, whose `store ptr null`
        // re-runs on every activation while the state dispatch jumps straight
        // past the capture site — so a read of the binding after the in-arm
        // `await` loaded null and dereferenced it (SIGSEGV @ 0, on every
        // compiler back to before #3484; the #3484 fall-back deliberately
        // declines this shape because the fall-back slot is the promoted binding
        // alloca, which is erased during promotion).
        //
        // Give the holder the SAME home the binding itself has, rather than a
        // fourth storage class: registration is keyed on the ORIGINAL slot's own
        // membership, so this fires exactly where the existing authority already
        // decided "this slot must survive a suspension and has no frame field".
        // The spill area is calloc'd, so the holder starts null — the guard's
        // first-pass state — and then PERSISTS, which is the repair.
        if (newSlot != slot && smStateStruct_ && smSelf_ &&
            std::find(asyncHandlerSpillSlots_.begin(),
                      asyncHandlerSpillSlots_.end(),
                      slot) != asyncHandlerSpillSlots_.end())
            asyncHandlerSpillSlots_.push_back(newSlot);
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
                // fix #3484 follow-up (§27.5 / §30.1, v0.283): that MOVE leaves
                // the PRE-BOXING alloca with no frame field, so it is not
                // restored on a resume — it is a fresh-stack alloca holding
                // whatever the new activation left there. Harmless while nothing
                // read it after a suspend; not harmless once #3484 made it the
                // fall-back address for a NULL holder, which is exactly the
                // state a conditionally-executed capture site leaves behind. The
                // shape then returned a plausible WRONG NUMBER (0 for every odd
                // `n` where §27.5 requires 2n) in place of the old SIGSEGV —
                // strictly worse, and invisible to every test in the tree.
                //
                // buildStateMachineFrame reserves a second field, of the local's
                // pre-boxing type, for exactly this. Give the original slot that
                // field so BOTH halves round-trip: the holder through the moved
                // ptr field, the pre-boxing alloca through its own.
                //
                // This is the LOCAL half of the same "one slot, two meanings"
                // problem the captured-PARAMETER branch above solves by giving
                // the HOLDER the new field. The halves are mirrored, not
                // identical, because only the parameter's pre-boxing alloca is
                // already restored (by the resume prologue's unconditional copy
                // from its capture field) — which is precisely why every #3466
                // row passed while this one did not.
                if (currentFn_ == smCellFrameFn_) {
                    auto pit = smLocalPreBoxField_.find(decl);
                    if (pit != smLocalPreBoxField_.end())
                        smAllocaToField_[slot] = pit->second;
                }
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
        // §27.5 / §28.9 (#969), read ONCE for this capture: was the captured
        // local declared in a scope STRICTLY SHALLOWER than this capture site?
        // `declScopeDepth_` is #969's own predicate; three consumers below need
        // it — the #3498 guarded-boxing admission, the #3508 deferred cell free,
        // and the #3752 deferred CONTENT release — and all three must read it at
        // the same depth (they run in this one block, one after the other), so
        // it is hoisted here rather than respelled per consumer.
        bool declaredShallower = false;
        std::size_t declScopeDepth = 0;
        {
            auto ddit = declScopeDepth_.find(static_cast<const void*>(decl));
            if (ddit != declScopeDepth_.end()) {
                declaredShallower = ddit->second < disposeStack_.size();
                declScopeDepth = ddit->second;
            }
        }
        // fix #4210 (§27.5 / §28.9, v0.298): add the !definitelyEscapingLambdas_
        // AND !declsCapturedByEscaping_ guards. For a NON-ESCAPING lambda with
        // declaredShallower=true (capturing an outer-scope local from inside a
        // loop), the original !declaredShallower check suppressed boxedCellFree,
        // and the envdtor path was also blocked (isSharedCellEnvdtorFreeOwner
        // false for non-escaping). The cell leaked. Adding
        // !definitelyEscapingLambdas_ lets boxedCellFree arm for non-escaping
        // lambdas even when declaredShallower=true. The !declsCapturedByEscaping_
        // guard is the second half: it prevents arming boxedCellFree when an
        // ESCAPING lambda also captures the same local (the same decl). In that
        // case, the non-escaping key (decl, nonEscLam) is still in
        // cellFreeEligible_ (cellFreeEligible_ is keyed by (decl, lam) pairs,
        // and the non-escaping pair is not in capturedByEscaping which is keyed
        // by (decl, escLam)), so without this guard boxedCellFree would arm for
        // the non-escaping lambda AND the escaping lambda's envdtor would free
        // the cell — a double-free. Since the lambda is non-escaping, boxedCellFree
        // fires exactly once (at the lambda's scope end), so there is no
        // double-free when !declsCapturedByEscaping_ holds. For escaping
        // lambdas, boxedCellFree is already suppressed (capturedByEscaping has
        // the key), so the definitelyEscapingLambdas_ guard is harmless there.
        if (!alreadyBoxed && !isForVar &&
            currentFn_ == cellFreeEligibleFn_ &&
            !definitelyEscapingLambdas_.count(static_cast<const void*>(&lam)) &&
            !declsCapturedByEscaping_.count(static_cast<const void*>(decl)) &&
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
            // #429 / #782: guarded boxing is normally excluded — a local
            // declared in an outer scope and captured by an inner-LOOP lambda
            // reuses one cell across iterations, so freeing it at the lambda
            // scope would corrupt later iterations. A CASE-ARM binding is
            // different: its lexical lifetime is the arm scope, and the
            // binding slot is re-established by pattern matching on each arm
            // entry. Its guarded cell can therefore be reclaimed at that arm
            // scope; the disposal path nulls the holder before a later loop
            // iteration can re-enter the arm.
            // fix #3435 (§27.5 / §28.9, v0.283): the `inlineN > 0` conjunct on
            // the `loopStack_.empty()` proof is dropped — the proof itself has
            // nothing to do with the capture being an inline owned array. What
            // makes a guarded cell unsafe to free at this scope is a BACKWARD
            // EDGE above the capture site: one cell reused by every iteration's
            // closure, so a free at the lambda's own scope would corrupt the
            // next pass. `loopStack_.empty()` IS that proof, for any capture:
            // no enclosing loop ⇒ boxLocalSlot's null-guarded allocation runs at
            // most once per invocation and exactly one closure holds the cell.
            //
            // Why the plain-`var` case reached here at all is the surprising
            // half. boxLocalSlot picks the fresh (non-guarded) strategy when a
            // StoreInst to the slot sits in the CURRENT basic block; for a local
            // whose declaration and capture are adjacent statements that is the
            // same block, which is why `var t := 0; var f := lambda() => t;` was
            // always clean. Put ANY block-splitting construct between them — the
            // issue's `for` loop, but equally an `if`, a `while`, or a `case` —
            // and the store is in a different block, the guarded path is taken,
            // and this exclusion silently dropped the cell on the floor. The
            // discriminator in the issue (clean with the assignment moved OUT of
            // the loop) is exactly that block boundary, not the assignment.
            //
            // The two hazards a widened admission could reintroduce are BOTH
            // already answered downstream, in consumeLastLambdaCapturedCells:
            // a disposal that READS THROUGH the cell defers the free into the
            // owner's scope (#966 / #1017), and a captured local declared in a
            // SHALLOWER scope than the capture site skips the free entirely
            // (#969) — the `do var g := () => x; end; return x;` read-after-
            // inner-scope case. So the free registered here fires at the
            // captured local's OWN scope end or not at all.
            // fix #3498 (§27.5 / §28.9, v0.283): `declaredShallower` is the
            // fourth admission, and it is not a new proof — it is the OBSERVATION
            // THAT THIS GATE IS ANSWERING A QUESTION ITS OWN CONSUMER ANSWERS
            // BETTER. The hazard the guarded-in-loop exclusion names, in its own
            // words, is "a local declared in an OUTER scope … captured by an
            // inner-LOOP lambda … freeing it at the lambda scope would corrupt
            // later iterations". But for exactly that shape
            // `consumeLastLambdaCapturedCells` never frees at the lambda scope:
            // it either DEFERS the free into the owner's scope (#966 / #1017,
            // when the captured local has a reads-through-cell disposal) or SKIPS
            // it outright (#969). So the exclusion suppresses only frees that
            // would have landed in the RIGHT place, and the shape it is actually
            // protecting — a local declared at the SAME dispose depth as an
            // in-loop capture site, whose one guarded cell really would be freed
            // once per iteration — is untouched, because `declaredShallower` is
            // false for it.
            //
            // `declScopeDepth_` is #969's OWN predicate, read here rather than
            // respelled, and it is read at the same depth the consumer will read
            // it at (both run inside this scope, one immediately after the
            // other), so the two cannot disagree.
            //
            // fix #3752 (§27.5 / §28.9 / §28.2, v0.286): the two values are now
            // computed ONCE, just above this block — the deferred CONTENT
            // release added by that fix needs the same depth fact and must read
            // it at the same point, so respelling it here would be a second
            // spelling of one question.
            bool guardedOk = !wasGuardedLocal || isPatternBinding ||
                             loopStack_.empty() || declaredShallower;
            if (guardedOk) {
                LastCellEntry lce{newSlot, static_cast<const void*>(decl),
                                  inlineN};
                // fix #2334: see the param-capture site above.
                lce.unwindUnsafe = unwindUnsafeCellFree_.count(
                                       static_cast<const void*>(decl)) != 0;
                lce.pushScopeDepth = disposeStack_.size();  // fix #4261 §28.3
                // fix #4165 (§27.5/§28.9, stdlib §4.8, v0.295): if the captured
                // local is a string-keyed `DictionaryEntry<string,V>` (bound to
                // d.First()/.Last()/.Single()/.ElementAt(i), per
                // isFreshDictEntryKeyReleaseTerminalLocal / #3720), the local's
                // rebuilt Key's single producer +1 was never discharged: the
                // normal-scope dictEntryKeyRelease disposal was NOT registered
                // for it (StringEscapeWalker treats ANY capture as an escape,
                // per that predicate's own documented trade-off), and the
                // envdtor path (freeCellInEnvdtor) requires
                // definitelyEscapingLambdas_ so it does not fire for a
                // non-escaping closure either. The cell gets freed by
                // boxedCellFree at scope end, but the cell holds the
                // DictionaryEntry struct BY VALUE with the rebuilt string Key
                // at field 0 — that Key leaks (32 B calloc'd `bz_string_t`
                // header + N B malloc'd payload, one of each per call, 500
                // calls → 500 × (32 + N) B). Set this flag so
                // consumeLastLambdaCapturedCells propagates it onto the
                // boxedCellFree Disposal as `dictEntryKeyReleaseDeferred` —
                // the FIRST PASS arm in emitDisposals already emits the GEP
                // field 0 + bz_string_release before the cell free under that
                // flag (set in #4165's pre-existing the `declaredShallower`
                // / boxedCellFreeDeferred arm).
                //
                // Same SingleDecl-only restriction as the `declaredShallower`
                // arm's identical check at line 2036 (LOCALDECL-DEBT comment):
                // captured-local map is `const void*`-keyed; `const` locals
                // never reach this arm. cellFreeEligible_ gating is preserved
                // by the surrounding `if (!alreadyBoxed && … &&
                // cellFreeEligible_.count(decl))` — `cellFreeEligible_` is
                // populated by collectCellFreeEligible's LambdaFinder, which
                // correctly distinguishes non-escaping from escaping captures.
                // LOCALDECL-DEBT: #4165 (LastCellEntry::dictEntryKeyReleaseDeferred new arm; const locals never carry DictionaryEntry<string,V>)
                if (auto* ld = dynamic_cast<const ast::LocalDecl*>(decl)) {
                    const sema::TypeRef* rt =
                        ld->type ? ctx_.resolvedType(*ld->type)
                                 : (ld->initializer
                                        ? ctx_.exprType(*ld->initializer)
                                        : nullptr);
                    if (rt && isDictEntryStringKeyTypeRef(rt))
                        lce.dictEntryKeyReleaseDeferred = true;
                }
                lastLambdaCapturedCells_.push_back(lce);
            }
            // fix #3508 (§27.5 / §28.9, v0.286): the #969/#3403 depth guard
            // at the CONSUMER refuses to free a captured cell at the inner
            // lambda's scope when the captured local was declared in an OUTER
            // scope (the read-after-inner-block hazard: the local's slot is
            // read THROUGH the cell after the inner block exits, when the
            // local outlives the inner block). For a SYNC lambda capture
            // behind an `if` (no async, no catch-arm, no loop), the lambda is
            // non-escaping (`cellFreeEligible_` says so) and `loopStack_
            // .empty()` says the guarded cell is allocated exactly once per
            // invocation. Move the free to the OWNER scope via
            // `boxedCellFreeDeferred` — the same machinery `consumeLast
            // LambdaCapturedCells` already uses for case-arm pattern
            // bindings, and it is sound for the same reason: every closure
            // holding the cell is dead by the owner's scope end (cellFree
            // Eligible_'s pre-scan), and the deferred fires AFTER every
            // reads-through-cell disposal in that scope (#1017 SECOND PASS in
            // LlvmBackend_Disposal.cpp) so the cell has no reader and no env
            // when the free runs. The local `k` of the issue's repro
            //
            //     var k := n*2; if … then var g := lambda() => k; s := g();
            //     end if; return s + k;
            //
            // leaks 2000 B / 500 without this; 0 with it.
            //
            // The existing path's `lastLambdaCapturedCells_.push_back(lce)`
            // is kept; the consumer's depth guard skips it (continue, not
            // free) for declaredShallower, leaving only this deferred free to
            // fire. No double-free: the deferred is gated on
            // `!alreadyDeferred` — the same dedup the consumer uses — and the
            // owner-scope read of `k` through the cell happens BEFORE the
            // deferred (at scope END), so no UAF on the local's own post-block
            // reads.
            //
            // Narrowing gates: !alreadyBoxed (a 2nd capture reuses the cell
            // and goes through the already-boxed path; that branch's owner is
            // named by sharedCellEnvdtorFreeOwner_, not here), !isForVar
            // (for-var captures reuse the cell across iterations; the
            // #3498 widening keeps the existing leak in that shape), and
            // !isPatternBinding (the consumer's depth-skip branch already
            // handles pattern bindings with its own deferral; this is the
            // outer-local analogue). loopStack_.empty() narrows to the
            // one-cell-per-invocation shape (a non-empty loop allocates
            // per-iteration guarded cells; deferring to owner scope would
            // only free the LAST iteration's cell).
            if (declaredShallower && loopStack_.empty() &&
                !alreadyBoxed && !isForVar && !isPatternBinding &&
                currentFn_ == cellFreeEligibleFn_ &&
                cellFreeEligible_.count(static_cast<const void*>(decl)) &&
                declScopeDepth > 0) {
                std::size_t ownerScopeIdx = declScopeDepth - 1;
                // The owner's `disposeStack_` slot must exist; the depth was
                // recorded at the local's declaration when that scope was
                // pushed, so it is on stack today.
                if (ownerScopeIdx < disposeStack_.size()) {
                    bool alreadyDeferred = false;
                    for (const auto& existing : disposeStack_[ownerScopeIdx])
                        if (existing.slot == newSlot &&
                            existing.boxedCellFreeDeferred) {
                            alreadyDeferred = true;
                            break;
                        }
                    if (!alreadyDeferred) {
                        Disposal dcd{newSlot, nullptr};
                        dcd.boxedCellFreeDeferred = true;
                        // fix #2334: mirror the TaskScope.Spawn unwind-
                        // unsafety of `lce.unwindUnsafe` (set above) onto
                        // the deferred entry so its `skipCellFreeOnUnwind`
                        // modifier matches.
                        dcd.skipCellFreeOnUnwind =
                            unwindUnsafeCellFree_.count(
                                static_cast<const void*>(decl)) != 0;
                        // fix #4165 (§27.5/§28.9, stdlib §4.8, v0.295): if the
                        // captured local is a string-keyed `DictionaryEntry<string,V>`
                        // (bound to d.First()/... and in cellFreeEligible_), the
                        // local's rebuilt Key's single producer +1 was never discharged:
                        // the normal-scope dictEntryKeyRelease disposal was NOT registered
                        // for it (StringEscapeWalker treats ANY capture as an escape,
                        // per that predicate's own documented trade-off), and the envdtor
                        // path (freeCellInEnvdtor) requires definitelyEscapingLambdas_
                        // so it does not fire for a non-escaping closure either. This
                        // flag parallels dictEntryKeyReleaseDeferred: in the SECOND
                        // PASS of emitDisposals, emit the same GEP field 0 +
                        // bz_string_release before the cell free. The type check
                        // (isDictEntryStringKeyTypeRef on the LocalDecl's resolved
                        // type) is identical to the #4127 escaping-case check;
                        // the safety case is the same: singleCaptureLocals_ means
                        // exactly one holder and exactly one +1 to balance.
                        //
                        // NOTE: we check cellFreeEligible_ (NOT dictEntryKeyReleaseEligible_)
                        // because StringEscapeWalker's ANY-capture-is-escape rule means
                        // dictEntryKeyReleaseEligible_ does NOT contain a captured local
                        // even when the capturing lambda is non-escaping. cellFreeEligible_
                        // is populated by collectCellFreeEligible's LambdaFinder, which
                        // correctly distinguishes non-escaping from escaping captures.
                        //
                        // Narrow (fix-forward of c1237c9f): drop dictEntryForVarSlot
                        // — the original fix's `dictEntryForVarSlot = locals_[key]`
                        // returned the REBOUND cell-holder ptr* alloca (rebound at
                        // Exprs2.cpp:1664 above) on this path too, not the original
                        // struct alloca. The emission site (LlvmBackend_Disposal.cpp
                        // SECOND PASS) now reads the cell's struct type from
                        // cellVars_[d.slot] (set on both boxing paths in
                        // boxLocalSlot), so the field is unused.
                        // LOCALDECL-DEBT: #4165 — captured-local map is `const void*`-keyed; `const` locals never reach this arm.
                        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(decl)) {
                            if (cellFreeEligible_.count(
                                    static_cast<const void*>(decl))) {
                                const sema::TypeRef* rt =
                                    ld->type ? ctx_.resolvedType(*ld->type)
                                             : (ld->initializer
                                                    ? ctx_.exprType(*ld->initializer)
                                                    : nullptr);
                                if (rt && isDictEntryStringKeyTypeRef(rt))
                                    dcd.dictEntryKeyReleaseDeferred = true;
                            }
                        }
                        disposeStack_[ownerScopeIdx].push_back(dcd);
                        if (inlineN > 0) {
                            Disposal da{newSlot, nullptr};
                            da.inlineOwnedArrayCell = true;
                            da.inlineSlotCount = inlineN;
                            disposeStack_[ownerScopeIdx].push_back(da);
                        }
                    }
                }
            }
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
        //
        // fix #2706: as in the captured-PARAMS loop, a cell the body WRITES and
        // that more than one env can share has BOTH its retain and its envdtor
        // release suppressed — the two would otherwise refer to different
        // strings. Note the flag is consulted for EVERY capture of such a
        // variable, not only the `alreadyBoxed` ones, because it is the ENVDTOR
        // release (which the first env also emits) that must go away.
        //
        // fix #4198: OR when `decl` is directly RETURNED BY NAME from this
        // function — capturedLocalReturnedByName's declaration has the full
        // argument. Independent of assignedCaptureVars_: a READ-ONLY capture
        // that is separately returned hits the identical heap-use-after-free
        // (the return's silent transfer and the envdtor's content release
        // both claim the frame's one reference), with no write anywhere in
        // the program.
        ce.suppressContentRcInEnvdtor =
            mutatedSharedCaptureCell(
                static_cast<const void*>(decl), ce.semaTy,
                /*freshCellPerIteration=*/forceFresh) ||
            capturedLocalReturnedByName(static_cast<const void*>(decl),
                                         ce.semaTy);
        // fix #3752 (§28.2 / §27.5 / §28.9, v0.286): THE FIRST capture's
        // envdtor is the captured local's DEFERRED SCOPE-END RELEASE — and a
        // deferred release is only sound when the env dies at or AFTER the
        // local's own scope end. That is exactly what `declaredShallower`
        // denies: the local was declared in a strictly SHALLOWER scope than
        // this capture site, so the closure holding this env is released at the
        // INNER scope's end while the local is still live and read afterwards.
        // The envdtor then frees the string/closure the local still owns —
        // measured as `heap-use-after-free ... freed by ... bz$lam$N$envdtor`
        // on
        //     var f := lambda(): int => 11;
        //     do var g := lambda(): int => f(); total := g(); end;
        //     return total + f();        // <- reads freed 40-byte cell
        // and, for the §28.2 shape issue 3752 was filed on, as a SIGSEGV under
        // `--checked-disposal` where the method-level twin raises a clean
        // DisposedAccessException: the backstop cannot fire through a closure
        // the envdtor already freed.
        //
        // The repair is the CONTENT analogue of what #3508 already does for the
        // CELL, under the very same gate and the very same proof: suppress the
        // early envdtor release and register ONE release in the OWNER's scope.
        // `cellFreeEligible_` is the proof that EVERY lambda capturing this
        // local is non-escaping, so every env is dead by the owner's scope end;
        // `declScopeDepth_` names that scope. Accounting is exact, not a leak
        // trade: the local's single +1 (its scope-end release suppressed by
        // escape detection) is released exactly once, at the scope where it
        // actually dies. 2nd-and-later captures are untouched (`!alreadyBoxed`)
        // — they mint their own +1 and their envdtors stay balanced whenever
        // they run.
        //
        // ORDER. The owner-scope entry is a MAIN-LOOP disposal, so it fires
        // before the SECOND-pass `boxedCellFreeDeferred` that frees the cell it
        // reads through (#1017's two-pass split), and after every inner scope
        // has run — i.e. after the last envdtor that could observe the value.
        const bool contentIsString = isStringTypeRefNonNull(ce.semaTy);
        const bool contentIsClosure =
            ce.semaTy && ce.semaTy->kind == sema::TypeRef::Kind::Function;
        // fix #3778 (§27.5 / §28.9, v0.286): widen the #3752 gate's
        // `declaredShallower` conjunct with `inArgumentLambda_` so a
        // CAPTURING CLOSURE PASSED AS AN ARGUMENT also triggers the
        // deferred CONTENT release. The argument closure is a TEMPORARY
        // (§27.5): the callee releases it at end of the call statement,
        // which sits inside the captured local's scope — so the envdtor's
        // content release fires before any later read of the local. Measured
        // on 3729220 (the #3752 fix):
        //     var f := lambda(): int => 11;
        //     var total := Apply(lambda(): int => f());  <- temporary lambda
        //     return total + f();                        <- reads freed cell
        // ASan: `heap-use-after-free ... in bz$M.C$Take`; `freed by ... in
        // bz$lam$1$envdtor`. The #3752 gate declined this because the
        // capture site is at the SAME dispose scope as `f` (`declaredShallower`
        // is false) and the pre-scan misclassifies argument-position
        // lambdas as escaping, so the local is NOT in `cellFreeEligible_`
        // on the bare parent.
        //
        // The repair keeps the CONTENT release (suppressed here, deferred
        // to the owner's scope — exactly what #3752 does for a deeper-
        // scope capture) and adds two new things that the new venue needs:
        //
        //   1. PRE-SCAN widening (§cellfree#3778, Setup.cpp). The argument
        //      closure does not actually outlive the call: it is consumed
        //      by the callee (a function-typed parameter, not stored in
        //      a field, not returned through a function-typed return).
        //      `isCallBoundedClosureArgPosition` is widened to admit non-
        //      function-typed returns so the captured local lands in
        //      `cellFreeEligible_`. The widening is deliberately narrower
        //      than the IIFE case: an async callee and a yield-body callee
        //      stay excluded (the closure may live on the awaited /
        //      iterated task), and an `owned`/`scoped` parameter stays
        //      excluded (those have their own storage-bounded admission).
        //
        //   2. GATE widening (this block). With the pre-scan correctly
        //      recognizing the argument lambda as non-escaping, the
        //      captured local is in `cellFreeEligible_`. The original
        //      #3752 gate further required `declaredShallower` — a CAPTURE
        //      IN A STRICTLY DEEPER SCOPE. The new venue is at the SAME
        //      scope as the local: the lifetime mismatch is a STATEMENT
        //      (end of the call expression), not a deeper dispose scope.
        //      `inArgumentLambda_` (set by `buildOrderedArgs` around
        //      `lowerExpr(*a.value)` for lambda arguments) is the signal:
        //      the closure dies at end of statement, before the local's
        //      scope end. Replacing `declaredShallower` with
        //      `(declaredShallower || inArgumentLambda_)` covers BOTH
        //      venues under the same one-shot registration; both releases
        //      stay exactly once per reference (one `+1` produced by the
        //      capture-time cell allocation, one release at the owner's
        //      scope end — never both).
        // fix #4261-FORWARD, narrowed (#3743-regression fix,
        // §28.3 / §27.5 / §28.9, v0.301): the original widening
        // fired for ANY captured-cell + reassigned local with
        // mutatedSharedCaptureCell=true, but #3743's `f` (declared
        // inside outer's body, reassigned by `mk`, captured by
        // outer's returned `t + f()`) is registered for content
        // release by the new arm WHILE the eager
        // `!isCapturedCell`-gated release at the call use site also
        // fires (closureLocalCapturedCell_ is populated only by
        // top-level LocalDecls in collectCapturedCellClosureLocals
        // — `f` is nested inside outer's body so it is NOT marked,
        // the eager arm fires, AND the new arm's deferred release
        // also runs at outer-scope end = double-free / heap
        // corruption). The discriminator is `closureLocalCapturedCell_`
        // itself: when populated, the eager arm is suppressed, and
        // this arm is the SOLE release (the accounting case
        // described below); when empty, the eager arm already
        // released and this arm must NOT add a second.
        //
        // Accounting proof (preserved from the un-narrowed #4261-FORWARD):
        // a captured-cell + reassigned local whose envdtor's content
        // release is SUPPRESSED (mutatedSharedCaptureCell suppresses
        // for the multi-env-shares-one-cell + body-writes combination)
        // would otherwise leak its cell content forever: every prior
        // reassign's old-value release balances the previous new value
        // via `lowerCapturedClosureLocalReassign`, but the FINAL
        // reassign's closure value remains held by the cell with no
        // envdtor decrementing it. The `declaredShallower` /
        // `inArgumentLambda_` shape fires #3752/#3778 for lifetime
        // reasons (the envdtor would run too early on the deeper-scope
        // capture); the SUPPRESSED envdtor case here fires for an
        // accounting reason (the envs share the cell — every envdtor
        // release would over-release the single +1). In both cases
        // exactly ONE release at the owner's scope (this block's
        // `disposeStack_[ownerScopeIdx]` push) is needed; the cell
        // holder's lifetime (the local at depth `declScopeDepth`)
        // ends before its `+1` content would otherwise die.
        if ((contentIsString || contentIsClosure) &&
            !alreadyBoxed && !isForVar && declScopeDepth > 0 &&
            currentFn_ == cellFreeEligibleFn_ &&
            cellFreeEligible_.count(static_cast<const void*>(decl)) &&
            ((declaredShallower || inArgumentLambda_) ||
             (closureLocalCapturedCell_.count(static_cast<const void*>(decl)) &&
              assignedCaptureVars_.count(static_cast<const void*>(decl)) &&
              mutatedSharedCaptureCell(static_cast<const void*>(decl),
                                       ce.semaTy,
                                       /*freshCellPerIteration=*/false)))) {
            std::size_t ownerScopeIdx = declScopeDepth - 1;
            if (ownerScopeIdx < disposeStack_.size()) {
                ce.suppressContentRcInEnvdtor = true;
                bool alreadyDeferred = false;
                for (const auto& existing : disposeStack_[ownerScopeIdx])
                    if (existing.slot == newSlot &&
                        existing.capturedCellContentRelease) {
                        alreadyDeferred = true;
                        break;
                    }
                if (!alreadyDeferred) {
                    Disposal dcr{newSlot, nullptr};
                    dcr.capturedCellContentRelease = true;
                    dcr.capturedCellContentString = contentIsString;
                    disposeStack_[ownerScopeIdx].push_back(dcr);
                }
            }
        }
        if (alreadyBoxed && !wasGuardedLocal && !ce.suppressContentRcInEnvdtor) {
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
        // scope is gone before the escaped closure dies (no UAF). For-var cells
        // remain excluded (per-iteration holders); a case-arm binding is the
        // scoped exception handled above.
        //
        // fix #3455 (§27.5 / §28.9, v0.283): `loopStack_.empty()` joins the
        // guarded-boxing exclusion here, which makes this the THIRD and LAST of
        // the four cell-reclaim arms in this function to state the SAME proof.
        // The captured-PARAM twin of this branch already reads
        // `(!wasGuardedParam || loopStack_.empty())` (#3403) and the captured-
        // LOCAL scope-end branch above already reads
        // `!wasGuardedLocal || isPatternBinding || loopStack_.empty()` (#3435);
        // only this arm still spelled the exclusion as a flat `!wasGuardedLocal`,
        // and a flat `!wasGuardedLocal` is not a statement about anything this
        // branch needs. What makes a guarded cell unsafe to free from ONE
        // envdtor is a BACKWARD EDGE above the capture site — one syntactic
        // lambda node inside a loop makes N closures over ONE guarded cell, and
        // N envdtors would free it N times. `loopStack_.empty()` IS that proof:
        // with no enclosing loop the null-guarded allocation runs at most once
        // per invocation, and `singleCaptureLocals_` then means exactly one
        // closure holds it.
        //
        // WHY THE OLD SPELLING DECLINED A SHAPE IT WAS MEANT TO COVER.
        // `wasGuardedLocal` reports boxLocalSlot's STRATEGY, which is chosen by
        // "is there a StoreInst to this slot in the CURRENT basic block?" —
        // a question about where the builder happens to be, not about this
        // capture. lowerLambda boxes captured PARAMETERS before captured LOCALS,
        // and a param that reaches boxLocalSlot's ALREADY-BOXED path emits the
        // #2303 null-guard, which splits the block. So in
        //
        //     var g := lambda() => k;      /* k boxed here, fresh */
        //     var t: int := g();
        //     var f := lambda() => k + t;  /* k re-boxed: guard splits the block */
        //     return f;                    /* ... so t is then GUARDED */
        //
        // `t` — captured by exactly ONE, definitely-escaping lambda, which is
        // precisely the shape #694 exists to handle — was pushed onto the
        // guarded path by `k`'s null-guard and declined. Measured 4 B/call
        // (2000 B / 500 allocations at x500) on ef339fa, clean with this
        // conjunct. Because captured locals are enumerated from an unordered_map,
        // WHICH capture arrives second is iteration order, so the old spelling
        // was also order-dependent.
        if (!alreadyBoxed &&
            (!wasGuardedLocal || isPatternBinding || loopStack_.empty()) &&
            !isForVar &&
            currentFn_ == boxCaptureLocalsFn_ &&
            definitelyEscapingLambdas_.count(static_cast<const void*>(&lam)) &&
            singleCaptureLocals_.count(static_cast<const void*>(decl)))
            ce.freeCellInEnvdtor = true;
        // fix #3426 (§27.5 / §28.9, v0.283): the SHARED-cell route — the issue's
        // own repro (`var g := () => k;` used, then `var f := () => k; return f;`
        // — 4 B for an `int` capture, 8 B for a `string`, order-independent).
        // Neither existing mechanism could reclaim that cell: `k` is in
        // capturedByEscaping so the scope-end boxedCellFree is (correctly)
        // disarmed, and `k` is captured by two envs so singleCaptureLocals_
        // (correctly) declines the branch above. sharedCellEnvdtorFreeOwner_
        // adds the missing third: it names the ONE definitely-escaping capturer
        // whose envdtor provably runs after every non-escaping sibling's — see
        // the field's declaration for why that ordering is guaranteed and why
        // the walker declines the shapes where it is not.
        //
        // `!isForVar` is kept (a for-var's cell is fresh per iteration, so no
        // single envdtor reclaims the earlier passes' cells) and
        // `loopStack_.empty()` is the backward-edge proof, but `!alreadyBoxed`
        // and the guarded-boxing exclusion are deliberately NOT: the shared cell
        // is usually created by the FIRST capture, so the escaping owner arrives
        // through boxLocalSlot's already-boxed path — which reports
        // wasGuardedLocal=false regardless of how the cell was allocated and so
        // is not a proof of anything at this site.
        // fix #4210 (§27.5 / §28.9, v0.298): add the definitelyEscapingLambdas_
        // guard. Without it, this condition fires for non-escaping lambdas too,
        // setting freeCellInEnvdtor=true when boxedCellFree also fires (after the
        // fix at line ~1798 arming boxedCellFree for non-escaping lambdas with
        // declaredShallower=true) — a double-free. The original
        // isSharedCellEnvdtorFreeOwner check was false for non-escaping lambdas
        // (sharedCellEnvdtorFreeOwner_ is only populated for definitelyEscaping),
        // which is why the original code leaked (boxedCellFree was suppressed by
        // declaredShallower=true for non-escaping) rather than double-freeing.
        // Reverting to isSharedCellEnvdtorFreeOwner restores the correct semantics:
        // this shared-cell route fires only for escaping lambdas where
        // capturedByEscaping has the key (boxedCellFree disarmed), and the
        // envdtor is the cell's sole reclaim path.
        //
        // fix #3435 follow-up: RESTORE `loopStack_.empty()` here. The original
        // #3435/#3426 commit (09132a99) included it, but #4210 (db2386a16)
        // dropped it in favour of the new `definitelyEscapingLambdas_` guard
        // — that guard covers a different hazard (non-escaping lambdas
        // falsely classified as cell owners) but does NOT cover this one. A
        // lambda captured inside a loop builds N closures over ONE cell
        // (singleCaptureLocals_ counts syntactic NODES, so the walk records
        // only the one owner, but iteration N produces N envs that all
        // release the cell). Without `loopStack_.empty()` each iteration's
        // envdtor calls `free(cell)` on the same cell — a double-free (the
        // canonical `field_store_escape_inside_loop_…` OVER-RELEASE GUARD row
        // of #3426's regression test dies with "AddressSanitizer: attempting
        // double-free" on the second iteration, measured on this build).
        // `definitelyEscapingLambdas_` is unchanged from #4210.
        if (!ce.freeCellInEnvdtor && !isForVar && loopStack_.empty() &&
            currentFn_ == cellFreeEligibleFn_ &&
            definitelyEscapingLambdas_.count(static_cast<const void*>(&lam)) &&
            isSharedCellEnvdtorFreeOwner(static_cast<const void*>(decl), &lam))
            ce.freeCellInEnvdtor = true;
        // fix #4509 (§27.5 / §28.9, v0.306): REFCOUNT this cell when — and only
        // when — every static route has declined it, which is exactly the set
        // that leaks today. The three declines, read off the sets that make
        // them rather than respelled:
        //
        //   * the SCOPE-END route refused because an escaping lambda captures
        //     the local (`declsCapturedByEscaping_`, #4210) and so it is not in
        //     `cellFreeEligible_` — an escaping env holds a raw cell pointer
        //     that MAY outlive the frame, which is a statement about what
        //     cannot be PROVEN, not about what happens;
        //   * #3426's shared-cell envdtor route refused (no
        //     `sharedCellEnvdtorFreeOwner_` entry);
        //   * #694's envdtor route refused — it needs the sole capturer to be
        //     DEFINITELY escaping, and `definitelyEscapingLambdas_`'s own
        //     condition (b) declines "a lambda merely passed as an argument"
        //     because it "proves nothing about when its env dies".
        //
        // All three are the same missing fact: WHEN the escaped env dies. That
        // is a runtime quantity, so answer it at runtime — the frame and every
        // env each hold a counted reference and the last one out frees the
        // cell. This is the narrow, cell-only half of the model #2718 sketches;
        // it deliberately does not touch the cell's CONTENTS accounting, which
        // is that issue's separate (and harder) half.
        //
        // SAFETY, stated as the property that makes this a narrow change: for
        // this set of cells today's codegen emits NO free at all — that is the
        // defect. So adding a balanced retain/release pair cannot turn a free
        // into a double free; the only arithmetic failure available is a
        // surviving leak, which is the PRIME DIRECTIVE's safe direction.
        // fix #4523: that claim is TRUE BUT INCOMPLETE, and the gap was a
        // deterministic SIGSEGV. The failure mode #4509 did not enumerate is
        // not a double free but an extra release registered into the WRONG
        // BODY's scope — see the classification below for how the frame's
        // release is now gated, and `declScopeDepthFn_` for why the depth alone
        // could never license the index.
        //
        // DELIBERATELY EXCLUDED, each a separate shape with its own reclaim
        // story: a for-var (fresh cell per iteration, #4129/#4161), an inline
        // owned array (the #747/#782 element-dispose-through-cell pairing), and
        // a captured PARAMETER (`declScopeDepth_` holds LOCALS only, per
        // #3403). A cell reached by the §27.5/#1135 NESTED-lambda widening is
        // NO LONGER excluded (fix #4523): its nested holder now announces
        // itself like any other env, which is what #4509's exclusion was
        // reaching for and could not express.
        // SCOPE OF THIS FIRST CUT, and the second half is measured, not
        // assumed. A capture whose CONTENTS the envdtor or the box machinery
        // also accounts for — a `string`/`string?`, a function type, a boxed
        // value (`releaseBoxInEnvdtor`), or one under #2706's
        // `suppressContentRcInEnvdtor` — is left alone. Two reasons, both
        // load-bearing:
        //
        //   * The cell's lifetime is entangled there with the CONTENTS
        //     refcount that #2718 leaves open ("rc(contents) must equal the
        //     LIVE ENV COUNT"), and #4509 is deliberately only the cell half.
        //   * Measured: for that shape the count cannot reach zero today
        //     anyway, so arming it reclaims nothing. On
        //     `const_narrowing_lambda_capture_value_2597` the escaping
        //     closure is itself never released (the #4422/#4431 family), so
        //     the cell keeps a live holder and `use_stacks=0` reports the
        //     SAME 64 B / 3 allocations with and without this fix — while the
        //     extra load+call+store perturbs the frame enough to move an
        //     unrelated pre-existing 48 B closure leak out of LSan's
        //     conservative stack scan and flip that row's one-shot oracle.
        //     Reclaiming nothing is not worth unmasking somebody else's leak,
        //     so it waits for #4509's follow-up alongside #2718.
        const bool contentsAlsoManaged =
            ce.releaseBoxInEnvdtor || ce.suppressContentRcInEnvdtor ||
            (ce.semaTy &&
             ((ce.semaTy->kind == sema::TypeRef::Kind::Primitive &&
               ce.semaTy->primitive == sema::PrimitiveType::String) ||
              ce.semaTy->kind == sema::TypeRef::Kind::Function));
        // fix #4523 (§27.5 / §28.9, v0.306): #4509's arming, restated as TWO
        // independent obligations instead of one.
        //
        //   (1) EVERY env that stores the cell pointer must announce itself —
        //       `bz_cell_retain` at its capture-store, `bz_cell_release` in its
        //       envdtor. That pair is SELF-BALANCING: it needs no
        //       `disposeStack_` entry, so it is correct in ANY body, including
        //       a nested lambda's.
        //   (2) The DECLARING FRAME's implicit reference (the one `calloc`
        //       created and nobody announced) must be dropped exactly once, by
        //       a `Disposal::cellRelease` in the scope that declared the local.
        //       That registration INDEXES `disposeStack_`, so it is only
        //       meaningful in the body whose `disposeStack_` the recorded depth
        //       came from.
        //
        // #4509 conflated them: it gated (1) on the same test as (2) and then
        // indexed `disposeStack_[declScopeDepth_[decl] - 1]` with a depth this
        // body may not own. `declScopeDepth_` is module-wide and never cleared
        // while `disposeStack_` is cleared per body, so for a lambda NESTED
        // inside another lambda that indexed the nested body's freshly-cleared
        // stack with the outer method's depth — an extra release into the wrong
        // function's scope, which nulls the holder and segfaults the next read
        // through it (#4523's repro). The numeric bound `<= disposeStack_.size()`
        // was the only thing standing between that and a wild index: an
        // arithmetic accident, not a proof.
        //
        // `declScopeDepthFn_` makes (2) fail-closed BY CONSTRUCTION — a depth
        // is only ever indexed into the body that recorded it — and (1) is then
        // free to be satisfied everywhere, which is what a refcount protocol
        // actually needs. #4509 instead declined nested holders, which is the
        // unsound direction: a declined holder reads through a cell the counted
        // holders may already have freed.
        //
        // The DECISION is per DECL and taken ONCE, in the declaring body:
        //   * a capture of a local can only appear in a lambda lexically inside
        //     that local's scope, and the outermost such lambda's capture loop
        //     runs while the declaring body is being lowered — before any
        //     lambda nested in it, and before any later sibling;
        //   * this loop over `capturedLocalDecls` has no early `continue`, so
        //     every captured local reaches the decision;
        //   * a refusal is STICKY (`cellRefcountDeclined_`) so a later sibling
        //     cannot arm a cell an earlier sibling's env already holds
        //     uncounted, and an arm is sticky (`refcountedCellDecls_`) so a
        //     later site cannot decline one the frame is already counting.
        // Params are absent from `declScopeDepth_` by construction (#3403), so
        // a captured parameter never reaches (2) and is never armed.
        const void* dkey = static_cast<const void*>(decl);
        // Is THIS body the one that declared the local, so that its recorded
        // depth indexes the `disposeStack_` we are holding right now?
        // fix #4526: this was #4523's own inline spelling of that test, kept
        // beside a separate `ddit->second - 1` read below. It is now the
        // single accessor both halves go through, so the check and the index
        // are one operation and no site can take the index without the proof.
        std::size_t ownerIdx = 0;
        const bool inDeclaringFrame = declScopeDisposeIndex(dkey, ownerIdx);
        bool armed = refcountedCellDecls_.count(dkey) != 0;
        if (!armed && inDeclaringFrame && !cellRefcountDeclined_.count(dkey)) {
            // The #4509 classification, unchanged except that
            // `nestedWidenedCellKeys_` is gone: a nested-widened cell is now
            // COUNTED by its nested holder rather than declined, which is what
            // that set existed to avoid.
            armed = !isForVar && !isInlineOwnedArray && !contentsAlsoManaged &&
                    !ce.freeCellInEnvdtor &&
                    currentFn_ == cellFreeEligibleFn_ &&
                    declsCapturedByEscaping_.count(dkey) &&
                    !cellFreeEligible_.count(dkey) &&
                    !sharedCellEnvdtorFreeOwner_.count(dkey) &&
                    !(singleCaptureLocals_.count(dkey) &&
                      definitelyEscapingLambdas_.count(
                          static_cast<const void*>(&lam)));
            if (!armed) cellRefcountDeclined_.insert(dkey);
        }
        if (armed) {
            // (1) — in every body, declaring or not.
            ce.retainCellInEnv = true;
            refcountedCellDecls_.insert(dkey);
            if (inDeclaringFrame) {
                // (2) — the frame's release goes in the scope that DECLARED the
                // local (`declScopeDepth_` records disposeStack_.size() at the
                // declaration, so the scope's index is one less), where the
                // #1017 SECOND PASS runs it after every reads-through-cell
                // disposal in that scope. One entry per cell holder, however
                // many lambdas capture it — the second capture reaches
                // boxLocalSlot's already-boxed path with the same `newSlot`,
                // so the dedup is an identity test on the holder. A generic
                // method re-lowered per specialization re-registers in each
                // specialization's own frame, which is why this is keyed on the
                // holder and not on `refcountedCellDecls_`.
                bool alreadyRegistered = false;
                for (const auto& existing : disposeStack_[ownerIdx])
                    if (existing.slot == newSlot && existing.cellRelease) {
                        alreadyRegistered = true;
                        break;
                    }
                if (!alreadyRegistered) {
                    Disposal dcr{newSlot, nullptr};
                    dcr.cellRelease = true;
                    disposeStack_[ownerIdx].push_back(dcr);
                }
            }
        }
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
    // §30.5 / #1503: the FULL declared `Task<T>`/`OwningTask<T>` annotation
    // (not just T), passed through to emitAsyncLambda so it can compute the
    // Pattern A result disposer. Null when no call-site annotation is visible
    // (the IIFE-invoked fallback below) — that shape gets no disposer, same
    // as any other unannotated async lambda before this fix.
    const sema::TypeRef* asyncDeclaredTaskTy = nullptr;
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
            asyncDeclaredTaskTy = taskTy;
            if (!taskTy->genericArgs.empty() && taskTy->genericArgs[0])
                asyncResultElemSemaTy = taskTy->genericArgs[0];
        } else if (retSemaTy && retSemaTy->kind != sema::TypeRef::Kind::Unit &&
                   retTy != i32()) {
            // fix #3789 (§30.6, v0.287): NO call-site annotation (e.g. an
            // IIFE-invoked async lambda). `retSemaTy` here is the wrapped
            // `Task<T>` (#800 Layer-2 explicitly reads `lamTy->funcReturn` for
            // an async expression body — the WRAPPER, not the element), so the
            // pre-fix pass-through silently made `asyncResultElemSemaTy` the
            // whole `Task<T>` (a Named type) where the call-site-annotation
            // path above extracted T via `genericArgs[0]`. That disagreement
            // reached `emitAsyncLambda` as `resultElemSemaTy`, which sized
            // `bz_task_complete`'s memcpy via
            // `getTypeAllocSize(lowerTypeRef(resultElemSemaTy))` — and for an
            // element T that is NOT a pointer (e.g. `int`),
            // `lowerTypeRef(Task<int>)` is `ptr` (8 B), so the producer
            // memcpy'd 8 bytes from a 4-byte `i32` value: `42` still came back
            // right (the consumer read 4 bytes from an 8-byte buffer and got
            // the low 4), but `1234567` SIGBUS'd when the over-read crossed
            // into another stack frame's live storage. For closure-typed T
            // the bug is silent: `lowerTypeRef(Task<() -> int>)` and
            // `lowerTypeRef(() -> int>)` are both `ptr`, so the two paths
            // produced the same size by coincidence (#3773 noted this).
            //
            // Mirror the annotation path exactly: extract T from
            // `retSemaTy->genericArgs[0]`. A `Task<T>` wrapping is the only
            // shape `retSemaTy` can carry here (it was read from
            // `lamTy->funcReturn` above, and an async lambda's funcReturn IS
            // `Task<T>` / `OwningTask<T>` per §30.6), so the generic arg IS T.
            // A void / Unit lambda body would have produced a null
            // `funcReturn` (or a `Kind::Unit` `retSemaTy`); the gate above
            // rejects both — leaving `asyncResultElemSemaTy` null, which is
            // the "no-value Task" case (per the no-annotation fallback's
            // contract below).
            asyncDeclaredTaskTy = retSemaTy;
            if (!retSemaTy->genericArgs.empty() && retSemaTy->genericArgs[0])
                asyncResultElemSemaTy = retSemaTy->genericArgs[0];
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
    // fix #4261 (§28.3, v0.298): sync path save of outer function's closure-
    // release eligibility. Declared here at outer scope so the restore in the
    // general restore section (below) is in scope. (Async path saves/restores
    // its own copy inside the `if (lam.isAsync)` block.)
    auto savedClosureRelEligOuter = closureReleaseEligible_;
    llvm::Function* savedClosureRelEligFnOuter = closureReleaseEligibleFn_;
    currentFn_ = fn;
    currentThis_ = nullptr;
    // #2515 / §27.3: a lambda body is NOT a method body — its `return` is an
    // internal transfer to the trampoline, not a method-contract return that
    // mints +1 / boxes against a declared return-type node.  Null it so
    // emitReturn's retain/box branches (keyed on currentReturnTypeNode_) do not
    // fire on the LEAKED enclosing-method return node.  The expr-body path
    // already uses retSemaTy (the lambda's own return type) for its retain
    // decisions; the block-body path now matches by carrying no declared node.
    // A lambda's external RC/boxing contract is settled at the CALL SITE from
    // the lambda's function type, not in the body.
    //
    // fix #2688 / §27.3 / §28.9: BOXING is settled at the call site, but the
    // refcount MINT is not — nulling the node also disabled emitReturn's
    // `string` +1 mint, which left the two body forms of the SAME lambda
    // disagreeing: `lambda(k) => k` minted +1 (the expr-body path below, keyed
    // on retSemaTy) while `lambda(k) => do return k; end` returned a +0 borrow.
    // Every caller that releases the result then double-freed the block-body
    // form only — a defect whose reachability depended on nothing but which
    // body form the author wrote. Carry the lambda's OWN return type into
    // emitReturn so the block-body path mints against retSemaTy exactly as the
    // expr-body path does. `retSemaTy` is already null for an async lambda
    // (cleared above — its declared return is the outer Task, minted by the
    // async machinery), so this is scoped to synchronous lambdas.
    const sema::TypeRef* savedLambdaRetSemaTy = currentLambdaReturnSemaTy_;
    currentLambdaReturnSemaTy_ = retSemaTy;
    currentReturnTypeNode_ = nullptr;
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
    // fix #2886: clear the caller's accessor-end block so a bare `return;`
    // inside the lambda body does not branch to the enclosing function.
    inlinedAccessorEnd_ = nullptr;
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

    // fix #2817 / #2807 / #429 / §4.6 / §28.9 / §28.3 / §27.3 (v0.240): run the
    // `var` STRING-local refcount analysis for the LAMBDA's own block body and
    // pin the token to the lambda's fn. EVERY consumer of the three string sets
    // gates on `currentFn_ == stringReleaseEligibleFn_` (armStringLocalRefcount's
    // bind-time retain AND its scope-end `Disposal{stringRelease}`,
    // returnedStringLocalOwnsPlusOne, the two record-backed interface-property
    // retains, and the `:=` / §4.6 `+=` drop-on-overwrite release), and until now
    // that token never named a lambda fn — the case armStringLocalRefcount's own
    // comment already recorded ("a lambda/thunk body lowered mid-function is not
    // walked, so the eligibility sets are empty for its locals — they just
    // leak"). Every `var` string local in every lambda body therefore leaked both
    // of its allocations, per call (#2817 — measured 1904 bytes / 80 allocations
    // for 20 invocations x 2 locals).
    //
    // WHY IT DOES NOT FIGHT THE ENV DESTRUCTOR. A lambda-body local captured by a
    // NESTED lambda is marked ESCAPED by StringEscapeWalker (`inLambda` makes any
    // read of the tracked local inside a lambda body an unconditional escape), so
    // it enters NEITHER release set and no scope-end release is registered for it
    // — exactly the suppression the envdtor's deferred release relies on. The
    // only thing that changes for such a local is that a BORROW-initialized one
    // now takes its bind-time retain (stringBorrowAll_), which is what the
    // envdtor's unconditional content release was already assuming was there;
    // that direction can only turn an over-release into a balanced pair.
    //
    // EXPRESSION bodies are excluded (no statements to analyze, no local to
    // release, and leaving the token unpinned keeps the expr-body return path's
    // `returnedStringLocalOwnsPlusOne` verdict bit-for-bit as it was). The async
    // branch below is handled inside emitAsyncLambda, against its RESUME fn.
    if (!lam.isAsync && !lam.exprBody) {
        collectStringReleaseEligible(lam.stmts);
        stringReleaseEligibleFn_ = fn;
        // fix #2696 / §28.1 / §28.9 (v0.240): CO-GATE the parameter retain with
        // the parameter release the pin above just turned on. The drop-on-
        // overwrite release admits ANY slot localSlot() resolves — including a
        // `string` PARAMETER, whose single reference belongs to the CALLER
        // (§28.1: on a `string`, "`owned` is a compile error: such values carry
        // no disposal obligation to transfer (§28.9)"). Enabling that release
        // without the paired bind-time retain frees the caller's borrow under it
        // — the #2696 use-after-free — so the two predicates move together, which
        // is the invariant retainAssignedStringParams' step 0 states. Emitted in
        // the lambda's ENTRY block, where the parameter binding loop above has
        // just stored each incoming argument, so it dominates every assignment in
        // the body and runs exactly once per INVOCATION.
        if (paramSyms) retainAssignedStringParams(lam.stmts, *paramSyms);
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
        // fix #3505 (§29.3 / §30.1 / §30.6 / §30.7, v0.283): one guard for the
        // WHOLE suspend/resume frame, in place of the 23 hand-written `savedX`
        // locals this block carried. emitAsyncLambda's StateMachineFrameScope
        // retracts every member `clearSmFrame()` lists; this restores every
        // member `clearSmFrame()` lists, and both are generated from
        // BEZEL_SM_FRAME_MEMBERS, so the two halves cannot drift apart. Five
        // members the hand-written list omitted — smForInSrcFields_,
        // iterElemSemaTy_, iterElemIsOwnedClass_, iterCurrentValOwnedField_,
        // iterIsOwningIter_ — are now restored; the last three are what a sync
        // OWNING iterator's later `yield return` reads to dispose the value the
        // consumer abandoned (§29.1, #2621), so an `async lambda` declared in an
        // iterator body used to cost every later yield that dispose.
        // (#3484 follow-up's four cell-frame members and #1503's Pattern A
        // disposer were on the old list and are on this one; nothing that was
        // saved before has stopped being saved.)
        SmFrameSaveScope smFrameSave(*this);
        // NOT part of the retracted frame (clearSmFrame does not touch it), so
        // it keeps its own save: emitAsyncLambda repopulates it for the lambda's
        // own handler spills.
        auto savedAsyncHandlerSpillSlots = asyncHandlerSpillSlots_;
        // §30.1 (#1521): SUPPRESS the enclosing method's owned-param-auto-dispose
        // frame across the nested lambda emission (the save itself is the scope's
        // job now). emitAsyncLambda does not publish this field (a lambda's own
        // `owned` params are not yet wired into this mechanism), so left alone it
        // would still point at the OUTER method's StateMachineFrame while
        // smStateStruct_/smSelf_ are swapped to the LAMBDA's own (differently
        // laid out) frame — emitAsyncComplete calls inside the lambda's own
        // Resume body would then GEP the outer frame's capture field indices into
        // the lambda's struct type, a type/index mismatch. Null it for the
        // duration of the nested call so the lambda's own completion sites see
        // "no owned-param frame" (a correct no-op).
        asyncOwnedParamFrame_ = nullptr;
        // fix #4118 (§30.5, v0.295): asyncChainedReturn_ / asyncBorrowedReturn_
        // are the §30.5 Pattern A return-kind detection flags — deliberately
        // NOT part of BEZEL_SM_FRAME_MEMBERS (LlvmBackend_impl.hpp's own
        // comment: "saved where they are needed, by name, because they are
        // not clearSmFrame()'s business"), so SmFrameSaveScope above does not
        // touch them. They are a TRANSIENT publish/consume channel: an
        // enclosing `return await <call>` sets one of them true BEFORE
        // lowering its operand, and reads it back (read-and-reset) AFTER —
        // see LlvmBackend_Assign.cpp's NestedBodyScope comment on fix #2731
        // item 1. When that operand is itself an async-lambda IIFE
        // (`return await (async lambda() => ...)()`), lowering it reaches
        // HERE and calls emitAsyncLambda for the NESTED lambda's own
        // machine — whose own completion (LlvmBackend_Async.cpp) does the
        // identical unconditional read-and-reset for ITSELF. Without a
        // save/reset/restore around that nested call, the nested body
        // inherits the OUTER's already-set flag (misclassifying its OWN
        // return kind and attaching the wrong runtime disposer call to
        // itself) and then clears it to false, so the outer's own read-back
        // sees `false` and ALSO misclassifies (its kind-2 chained return
        // looks like a plain kind-1 return) — the two kind decisions swap.
        // This mirrors fix #2747's identical treatment in the SYNC branch
        // below (the sync branch's own nested body reads/resets the exact
        // same flags via emitReturn) and NestedBodyScope's by-name handling —
        // added here by name, NOT by folding into BEZEL_SM_FRAME_MEMBERS,
        // because these two are DESTRUCTIVE (they select which
        // bz_task_set_chained / bz_task_set_result_disposer call is emitted,
        // not a soft hint), and folding them into the macro would also
        // reshape NestedBodyScope's already-correct-by-name handling of the
        // identical pair into duplicate macro-generated members — out of
        // scope here per LANE_RULES coordination with the #4143 lane.
        bool savedAsyncChainedReturn = asyncChainedReturn_;
        bool savedAsyncBorrowedReturn = asyncBorrowedReturn_;
        asyncChainedReturn_ = false;
        asyncBorrowedReturn_ = false;
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
        // fix #3757 follow-up: closureLocalCandidates_ travels with the pair
        // above (see its declaration, LlvmBackend_impl.hpp).
        auto savedClosureLocalCandidates = closureLocalCandidates_;
        auto savedCellFreeElig = cellFreeEligible_;
        llvm::Function* savedCellFreeEligFn = cellFreeEligibleFn_;
        // fix #3426: the sole-escaping-holder map is recomputed for the
        // lambda's own body by emitAsyncLambda and keyed to the lambda
        // resume fn, exactly like cellFreeEligible_ above — so it needs
        // the identical save/restore, or a LATER lambda in the enclosing
        // body would be matched against the nested lambda's owners.
        auto savedSharedCellOwner = sharedCellEnvdtorFreeOwner_;
        auto savedBoxCaptureLocals = boxCaptureLocals_;
        auto savedSingleCaptureLocals = singleCaptureLocals_;
        auto savedAssignedCaptureVars = assignedCaptureVars_;  // fix #2706
        auto savedReturnedCaptureVars = returnedCaptureVars_;  // fix #4198
        llvm::Function* savedBoxCaptureLocalsFn = boxCaptureLocalsFn_;
        auto savedDefinitelyEscaping = definitelyEscapingLambdas_;
        // #2132: emitAsyncLambda now ALSO recomputes the borrowing-array /
        // dict-materializer / grouping scope-end reclaim eligibility for the
        // lambda body and keys it to the lambda resume fn. Save/restore it exactly
        // like the closure-release sets above — otherwise a collection LITERAL /
        // materializer local in the ENCLOSING async body that follows this
        // `var f := <lambda>` would be registered against the lambda's stale
        // eligibility (wrong set + wrong fn key) and leak.
        auto savedBorrowingArrayFreeElig = borrowingArrayFreeEligible_;
        llvm::Function* savedBorrowingArrayFreeEligFn = borrowingArrayFreeEligibleFn_;
        auto savedDictMaterializerFreeElig = dictMaterializerFreeEligible_;
        llvm::Function* savedDictMaterializerFreeEligFn = dictMaterializerFreeEligibleFn_;
        auto savedGroupingFreeElig = groupingFreeEligible_;
        llvm::Function* savedGroupingFreeEligFn = groupingFreeEligibleFn_;
        // fix #3720: same save/restore for the DictionaryEntry-Key-release
        // eligibility set — populated by the SAME collectBorrowingArrayFree
        // Eligible call this block already protects, so it needs identical
        // protection against the lambda's stale keys leaking into the
        // enclosing body's registration (LANE_RULES §8).
        auto savedDictEntryKeyReleaseElig = dictEntryKeyReleaseEligible_;
        llvm::Function* savedDictEntryKeyReleaseEligFn =
            dictEntryKeyReleaseEligibleFn_;
        // #2142 / #2143: same save/restore for the Set-literal / Dict-literal
        // reclaim eligibility sets — populated by the SAME
        // collectBorrowingArrayFreeEligible call emitAsyncLambda makes above, so
        // they need the identical protection against the lambda's stale keys
        // leaking into the enclosing body's registration.
        auto savedSetLiteralFreeElig = setLiteralFreeEligible_;
        llvm::Function* savedSetLiteralFreeEligFn = setLiteralFreeEligibleFn_;
        auto savedDictLiteralFreeElig = dictLiteralFreeEligible_;
        llvm::Function* savedDictLiteralFreeEligFn = dictLiteralFreeEligibleFn_;
        // fix #3880: same save/restore for the dict-backed TakeCurrent()
        // reclaim eligibility set — populated by the SAME
        // collectBorrowingArrayFreeEligible call emitAsyncLambda makes above,
        // so it needs identical protection (LANE_RULES §8).
        auto savedTakeCurrentDictFreeElig = takeCurrentDictFreeEligible_;
        llvm::Function* savedTakeCurrentDictFreeEligFn =
            takeCurrentDictFreeEligibleFn_;
        // fix #2660: same protection for the nested-literal aggregate set —
        // emitAsyncLambda recomputes it for the lambda body, and without the
        // restore the enclosing body's later aggregate roots would consult the
        // LAMBDA's set. (That direction only loses a free — a leak — but the
        // set must track its guard, dictLiteralFreeEligibleFn_, which IS
        // restored two lines below.)
        auto savedLiteralTreeAggElig = literalTreeAggregateEligible_;
        // fix #5106 follow-up: moves with literalTreeAggregateEligible_ —
        // same producer (collectBorrowingArrayFreeEligible), same guard.
        auto savedDictFlatValueFreeSafe = dictFlatValueFreeSafe_;
        // fix #3728 (§15.1.1 / §15.3 / §28.9, v0.286): emitAsyncLambda now ALSO
        // recomputes the §15.3 move-only owning inline-array TRANSFER set for
        // the lambda body and keys it to the lambda resume fn (the same
        // re-establishment #3699 gave the three state-machine emitters). Save
        // and restore it exactly like the borrowing-collection sets above — the
        // §8 "clear() + nested-emission save/restore are two halves of ONE
        // contract" rule (LANE_RULES) — or a LATER `[N owned]T` local in the
        // ENCLOSING async body would be tested against the lambda's stale
        // transfer set (wrong fn key): the mismatch fails safe (a leak, per the
        // guard's own "conservatively-misjudged transfer at worst LEAKS"), but
        // is still wrong and is exactly the class of defect this run's dedup
        // discipline exists to prevent.
        auto savedMoveOnlyArrayTransferred = moveOnlyArrayTransferred_;
        llvm::Function* savedMoveOnlyArrayTransferredFn =
            moveOnlyArrayTransferredFn_;

        emitAsyncLambda(lam, fn, paramSyms, asyncResultElemSemaTy,
                       asyncDeclaredTaskTy);

        closureReleaseEligible_ = std::move(savedClosureRelElig);
        closureReleaseEligibleFn_ = savedClosureRelEligFn;
        closureLocalCandidates_ =
            std::move(savedClosureLocalCandidates);  // fix #3757 follow-up
        cellFreeEligible_ = std::move(savedCellFreeElig);
        cellFreeEligibleFn_ = savedCellFreeEligFn;
        sharedCellEnvdtorFreeOwner_ = std::move(savedSharedCellOwner);  // #3426
        boxCaptureLocals_ = std::move(savedBoxCaptureLocals);
        singleCaptureLocals_ = std::move(savedSingleCaptureLocals);
        assignedCaptureVars_ = std::move(savedAssignedCaptureVars);  // fix #2706
        returnedCaptureVars_ = std::move(savedReturnedCaptureVars);  // fix #4198
        boxCaptureLocalsFn_ = savedBoxCaptureLocalsFn;
        definitelyEscapingLambdas_ = std::move(savedDefinitelyEscaping);
        borrowingArrayFreeEligible_ = std::move(savedBorrowingArrayFreeElig);
        borrowingArrayFreeEligibleFn_ = savedBorrowingArrayFreeEligFn;
        dictMaterializerFreeEligible_ = std::move(savedDictMaterializerFreeElig);
        dictMaterializerFreeEligibleFn_ = savedDictMaterializerFreeEligFn;
        groupingFreeEligible_ = std::move(savedGroupingFreeElig);
        groupingFreeEligibleFn_ = savedGroupingFreeEligFn;
        dictEntryKeyReleaseEligible_ =
            std::move(savedDictEntryKeyReleaseElig);                       // #3720
        dictEntryKeyReleaseEligibleFn_ = savedDictEntryKeyReleaseEligFn;    // #3720
        setLiteralFreeEligible_ = std::move(savedSetLiteralFreeElig);      // #2142
        setLiteralFreeEligibleFn_ = savedSetLiteralFreeEligFn;             // #2142
        dictLiteralFreeEligible_ = std::move(savedDictLiteralFreeElig);    // #2143
        dictLiteralFreeEligibleFn_ = savedDictLiteralFreeEligFn;           // #2143
        takeCurrentDictFreeEligible_ =
            std::move(savedTakeCurrentDictFreeElig);                      // #3880
        takeCurrentDictFreeEligibleFn_ = savedTakeCurrentDictFreeEligFn;   // #3880
        literalTreeAggregateEligible_ =
            std::move(savedLiteralTreeAggElig);                            // #2660
        dictFlatValueFreeSafe_ =
            std::move(savedDictFlatValueFreeSafe);        // #5106 follow-up
        // fix #3728: restore half of the save two-step above.
        moveOnlyArrayTransferred_ = std::move(savedMoveOnlyArrayTransferred);
        moveOnlyArrayTransferredFn_ = savedMoveOnlyArrayTransferredFn;
        // fix #3505: the whole sm*_/async*_ frame is put back by smFrameSave's
        // destructor at the end of this block. Only the members that are NOT
        // part of the retracted frame are restored by hand.
        asyncHandlerSpillSlots_ = std::move(savedAsyncHandlerSpillSlots);
        // fix #4118: restore the §30.5 Pattern A flags the save/reset above
        // set aside — see the comment there.
        asyncChainedReturn_ = savedAsyncChainedReturn;
        asyncBorrowedReturn_ = savedAsyncBorrowedReturn;
    } else {
    // fix #3237: a SYNC lambda body nested inside an async method also needs
    // the enclosing machine's sm*_/async*_ fields cleared — the sync lambda
    // body emit's `return value` lowers through emitReturn, and emitReturn
    // routes through emitAsyncComplete whenever `smSelf_` is non-null (which
    // it is here, set to the enclosing async resume fn's frame param). The
    // pre-fix path produced e.g. `bz_task_complete(... %choice.CInt ...)`
    // inside a `() -> int` lambda whose `%1` is actually the user's
    // CInt param — verifier `Referring to an argument in another function!`
    // for `getelementptr %async.Main, ptr %1, …` because `%1` is the
    // lambda's CInt arg, not the enclosing machine's frame. The async
    // branch above already does the same save/restore around emitAsyncLambda;
    // mirror it (with the FULL sm*_/async*_ set so a sync body containing a
    // nested iterator doesn't leak the iterator's frame back to the
    // enclosing machine either — review verdict item 4) for the sync body so
    // the enclosing machine's state is fully out of scope during
    // lowerScopedBlock(lam.stmts) / lowerExpr(exprBody).
    // fix #3505 (§29.3 / §30.1 / §30.6 / §30.7, v0.283): the sync twin of the
    // async branch's guard — one scope for the whole retracted frame, in place
    // of the hand-written `savedXSync` list. Nothing this branch saved before
    // has stopped being saved; what it gains is the five members the two lists
    // both omitted, which matters here because the sync body can itself contain
    // a nested `async lambda` or a call that instantiates a generic state
    // machine, either of which runs clearSmFrame while this body is mid-emission.
    //
    // Note the asymmetry with the async branch, which is deliberate: this branch
    // CLEARS only a subset of the frame (below), because a sync lambda body must
    // still see the enclosing machine's #3484 cell-frame publication when it
    // boxes a capture. The scope only saves; it does not clear.
    SmFrameSaveScope smFrameSaveSync(*this);
    auto savedAsyncHandlerSpillSlotsSync = asyncHandlerSpillSlots_;
    // fix #2747: the SYNC lambda body's `lowerReturnStmt` reads and RESETS the
    // outer `return await` Pattern A detection flags (`asyncChainedReturn_` /
    // `asyncBorrowedReturn_`) at lines 1928-1937 unconditionally — even when
    // the inner `return` is NOT itself an `await`. If the enclosing async
    // method's `return await <call>` already set these flags BEFORE entering
    // the lambda body (the lambda sits in the operand of the awaited call,
    // e.g. `return await C.Inner(C.Apply(lambda(...) => do return x + 1; end,
    // 6))`), the inner ReturnStmt reads the outer's flag and clobbers it back
    // to false, so the outer return-statement sees a false flag and treats
    // its chained-return slot as a plain borrow — the source task gets
    // free-on-completion (it should have been chained) and the caller sees
    // a freed cell on its own scope-end dispose (SEGV). Save and reset them
    // across the sync body so the inner ReturnStmt sees a clean slate.
    bool savedAsyncChainedReturnSync = asyncChainedReturn_;
    bool savedAsyncBorrowedReturnSync = asyncBorrowedReturn_;
    smKind_ = SuspendKind::Yield;  // placeholder (no neutral value, mirrors
                                    // NestedBodyScope / getter reset pattern;
                                    // smSelf_=null makes emitAsyncComplete
                                    // no-op regardless of smKind_ value)
    smStateStruct_ = nullptr;
    smSelf_ = nullptr;
    smStateGEP_ = nullptr;
    smParkedStateGEP_ = nullptr;  // fix #3531
    smStateBBs_.clear();
    smDoneBB_ = nullptr;
    smNextSuspendState_ = 0;
    smLocalFields_.clear();
    smAllocaToField_.clear();
    iterCurrentValField_ = 0;
    iterElemTy_ = nullptr;
    asyncOuterTaskField_ = 0;
    asyncPendingTaskField_ = 0;
    asyncSpillPtrField_ = 0;
    asyncHandlerSpillSlots_.clear();
    asyncResumeFn_ = nullptr;
    asyncResultTy_ = nullptr;
    asyncResultSemaType_ = nullptr;
    asyncPatternAResultDisposer_ = nullptr;
    asyncOwnedParamFrame_ = nullptr;
    asyncChainedReturn_ = false;
    asyncBorrowedReturn_ = false;
    // fix #3738 (§15.1.1 / §15.3 / §27.5, v0.286): pin the §15.3 (#747)
    // move-only owning inline-array TRANSFER analysis to the SYNC lambda's
    // own function, exactly as #3699 gave the three state-machine emitters
    // and #3728 gave the async-lambda twin (the branch above). Without this,
    // registerInlineOwnedArrayDisposal's `currentFn_ != moveOnlyArrayTransferredFn_`
    // guard (LlvmBackend_Setup.cpp) is unconditionally true for the whole sync
    // lambda body — every `[N owned]T` / `[N owned]T?` local in it NEVER
    // registers a scope-end disposal at all (a LEAK, never a double-free; the
    // guard's own documented direction is fail-safe). MoveOnlyArrayTransferScope
    // (LlvmBackend_impl.hpp, reused as-is per LANE_RULES §6 — no new
    // predicate) saves/restores the pair, so a lambda/iterator/state-machine
    // nested inside THIS body that re-pins the token for ITS OWN function
    // leaves this scope's copy correct when control returns here — the same
    // two-halves clear()+save/restore contract LANE_RULES §8 requires.
    // Expression bodies have no LocalDecl statements (so no `[N owned]T`
    // local of their own); clear the set for them, mirroring the async
    // branch's `!lam.exprBody` guard exactly.
    MoveOnlyArrayTransferScope moveOnlyArrayScopeSync(*this);
    if (!lam.exprBody)
        collectMoveOnlyArrayTransferred(lam.stmts);
    else
        moveOnlyArrayTransferred_.clear();
    moveOnlyArrayTransferredFn_ = fn;
    // fix #3757 (§27.5 / §15.3, v0.286): pin the closure-capture-cell
    // disposal family — closureReleaseEligible_ (#422), boxCaptureLocals_ /
    // singleCaptureLocals_ / assignedCaptureVars_ / definitelyEscapingLambdas_
    // (§27.5 / #672 / #694 / #2706), and cellFreeEligible_ / unwindUnsafeCellFree_
    // / sharedCellEnvdtorFreeOwner_ (§27.5 / #429 / #2334 / #3426) — to the
    // SYNC lambda's own function, mirroring emitAsyncLambda's identical
    // clear+collect+pin block for its resume fn (LlvmBackend_Async.cpp
    // ~L1943-1963) and Setup.cpp's block for a plain method body
    // (~L9197-9228). Sibling family to the #3738 fix immediately above
    // (moveOnlyArrayTransferredFn_): that fix's file ownership was narrowly
    // the §15.3 transfer token; this is the closure-capture-cell family,
    // never re-pinned for the sync-lambda venue at all. Without this,
    // collectCellFreeEligible/collectClosureReleaseEligible's admission
    // tests run against whatever the ENCLOSING function last pinned
    // cellFreeEligibleFn_/closureReleaseEligibleFn_ to, so a `currentFn_ !=
    // …Fn_` guard mismatch fails the eligibility check for every capture in
    // this body — a `[N owned]T` local captured by a NESTED lambda inside
    // this sync lambda's own body never disposes through its capture cell
    // (measured total=600 n=400 at 200 activations — a leak, not a
    // double-free per LSan). CaptureReclaimSetScope (LlvmBackend_impl.hpp,
    // added by fix #3500, reused as-is per LANE_RULES §6 — no new
    // predicate/scope) is the EXACT save/restore this family already needed
    // for `emitIteratorMethod` / `emitAsyncMethod` / `emitAsyncIteratorMethod`
    // when THOSE bodies nest a specialization mid-emission; it is the
    // identical hazard here — a lambda/iterator/state-machine nested inside
    // THIS sync lambda's own body that re-pins the same tokens for ITS OWN
    // function must leave this scope's copy correct when control returns
    // here (LANE_RULES §8's two-halves clear()+save/restore contract).
    // Must run collectCellFreeEligible AFTER collectClosureReleaseEligible
    // (uses its result) — same ordering emitAsyncLambda and Setup.cpp both
    // observe. Expression bodies have no LocalDecl/capture statements of
    // their own; clear closureReleaseEligible_ for them, mirroring the
    // async branch's `!lam.exprBody` guard exactly (cellFreeEligible_ /
    // boxCaptureLocals_ are unconditionally cleared+re-pinned either way,
    // since collectCellFreeEligible/collectBoxCaptureLocals over an empty
    // `lam.stmts` would be a no-op collection, not a stale one — but an
    // expr body never reaches this point with non-empty stmts to collect
    // over, so the clear is what matters).
    CaptureReclaimSetScope captureReclaimScopeSync(*this);
    boxCaptureLocals_.clear();
    // fix #3823 / §27.5 / §28.9 (v0.287): for an EXPR-body lambda whose body is
    // itself a LambdaExpr (Shape B/C: a closure-returning lambda whose expr body
    // is another lambda), DO NOT clear `singleCaptureLocals_` here — the
    // enclosing function's pre-scan set is the correct data for the nested
    // lambda's `capturedParamSyms` gate (line ~1405), because every lambda in
    // this body's enclosing function that captures any of THIS lambda's params
    // must, transitively, live inside THIS lambda's body (a lambda's own param
    // is not accessible outside its body). The count of captures from
    // `singleCaptureLocals_` keyed by this lambda's own param is therefore
    // invariant under the body-vs-enclosing-function view. Clearing it here
    // wipes Main's pre-scan tally before `lowerExpr(lam.exprBody)` reaches the
    // INNER's captured-param gate, and the gate then fails (singleCap=0) — the
    // exact 4-byte env n-field residual in #3823. For block bodies, the
    // `else`-branch's `collectBoxCaptureLocals(lam.stmts)` repopulates the
    // set, so the clear-then-repopulate pair is preserved for that case.
    if (!lam.exprBody) {
        singleCaptureLocals_.clear();
    }
    assignedCaptureVars_.clear();          // fix #2706
    // fix #4226 (§27.5 / §28.9, v0.298): snapshot the ANCESTOR scope's
    // already-computed returnedCaptureVars_ before it is cleared for THIS
    // nested lambda's own re-scan — see the merge just below (after the
    // #3804 sharedCellEnvdtorFreeOwner_ merge, same pattern) for the full
    // argument. Without this, a local declared and returned TWO OR MORE
    // scopes up (e.g. `f` in a grandparent function, `return f;` there, `f`
    // captured transitively by an intermediate lambda purely to hand it down
    // to a lambda nested inside THAT lambda) loses the fact once the
    // intermediate lambda's own collectBoxCaptureLocals rescan clears and
    // repopulates returnedCaptureVars_ from ONLY its own top-level `return`
    // statements (there are none — the intermediate lambda never itself
    // returns `f`) — so the doubly-nested lambda's own capture-loop entry for
    // `f` (consulted while currentFn_ == boxCaptureLocalsFn_ == the
    // INTERMEDIATE lambda's fn) sees an empty set and never suppresses its
    // own envdtor's content release, freeing `f`'s cell while the value is
    // still needed by the true caller (issue #4226; #4198 only reached the
    // one-level case, where the returning function IS the one whose
    // collectBoxCaptureLocals call sees `f`'s capturer directly).
    const std::unordered_set<const void*> ancestorReturnedCaptureVars4226 =
        returnedCaptureVars_;
    returnedCaptureVars_.clear();          // fix #4198
    // fix #4240 (§27.5 / §28.9, v0.298): snapshot the ANCESTOR scope's
    // already-computed multiCaptureLocals_ before collectBoxCaptureLocals
    // (below) clears+recomputes it for THIS nested lambda's own re-scan — see
    // the merge just below (after the #4226 returnedCaptureVars_ merge, same
    // pattern) and multiCaptureLocals_'s own declaration for the full
    // argument. Without this, a cell shared by an ANCESTOR env (which holds
    // a reference PURELY to hand it down to a lambda nested inside it,
    // without itself appearing in `lam.stmts`'s own collectAllLambdas result)
    // is undercounted by exactly the ancestor's own +1 contribution, so a
    // truly 2+-env cell looks single-captured from this narrower rescan —
    // which flips mutatedSharedCaptureCell's verdict on a WRITTEN cell from
    // "suppress both retain and release" to "suppress neither", unbalancing
    // the pair (issue #4240: heap-use-after-free, `freed by ... envdtor`, on
    // a mutated-not-returned two-level-nested capture).
    const std::unordered_set<const void*> ancestorMultiCaptureLocals4240 =
        multiCaptureLocals_;
    definitelyEscapingLambdas_.clear();
    boxCaptureLocalsFn_ = fn;
    cellFreeEligible_.clear();
    unwindUnsafeCellFree_.clear();         // fix #2334
    // fix #3804 (§27.5 / §15.3 / §28.9, v0.294): snapshot the ANCESTOR
    // scope's already-computed sharedCellEnvdtorFreeOwner_ before it is
    // cleared for THIS nested lambda's own re-scan — see the merge just
    // below (after collectCellFreeEligible) for the full soundness
    // argument.
    const std::unordered_map<const void*, const void*>
        ancestorSharedCellOwner3804 = sharedCellEnvdtorFreeOwner_;
    sharedCellEnvdtorFreeOwner_.clear();   // fix #3426
    cellFreeEligibleFn_ = fn;
    if (!lam.exprBody) {
        collectClosureReleaseEligible(lam.stmts);
        collectBoxCaptureLocals(lam.stmts);
        collectDefinitelyEscapingLambdas(lam.stmts);
        // Must run AFTER collectClosureReleaseEligible (uses its result).
        // fix #4524 (§27.5 / §28.9, v0.306): `&lam` marks this as a NESTED
        // LAMBDA's own re-scan, which may not elect a #3426 shared-cell owner
        // for a key it does not itself bind — see the parameter's comment.
        collectCellFreeEligible(lam.stmts, &lam);
    } else {
        closureReleaseEligible_.clear();
        // fix #3757 follow-up: closureLocalCandidates_ must clear alongside
        // closureReleaseEligible_ for the SAME reason an expr body clears the
        // set above — collectClosureReleaseEligible (which would otherwise
        // repopulate both together) never runs for an expr body, so leaving
        // this one stale would let a returned identifier that merely CAPTURES
        // an outer closure-typed local pass returnedClosureLocalOwnsPlusOne's
        // body-membership guard on a leftover hit from whatever body last
        // populated it (typically the ENCLOSING scope's own scan) — the exact
        // hazard that guard exists to close, reopened by an incomplete clear.
        closureLocalCandidates_.clear();
        // fix #3823 / §27.5 / §28.9 (v0.287): if the EXPR body IS another
        // lambda expression, that lambda IS this lambda's return value (its
        // sole expression), so it DEFINITELY ESCAPES this lambda's frame.
        // Mark it in definitelyEscapingLambdas_ so its envdtor can free a
        // sole-holder capture cell. The cell-free gate at LlvmBackend_
        // Exprs2.cpp:~1403 requires this admission; without it, a value-typed
        // capture cell (e.g. a captured int parameter `n` in
        // `lambda(n: int): (int) -> int => lambda(m: int) => n + m`) leaks
        // 4 B per call (the 4-byte env n-field residual in lam$0, #3823,
        // Shape C: `var add5 := adder(5);` and Shape B:
        // `var got := pick(n);`).
        if (lam.exprBody) {
            if (auto* innerLam = dynamic_cast<const ast::LambdaExpr*>(
                    lam.exprBody.get())) {
                definitelyEscapingLambdas_.insert(
                    static_cast<const void*>(innerLam));
            }
        }
    }
    // fix #3804 (§27.5 / §15.3 / §28.9, v0.294): merge the ANCESTOR scope's
    // sharedCellEnvdtorFreeOwner_ decisions back in for any key THIS body's
    // own re-scan does not know about. A captured FREE VARIABLE from an
    // ancestor scope (e.g. `outer`'s own captured `x`, boxed only to pass
    // down to a nested `inner`) is never itself a LocalDecl of THIS body,
    // so collectCellFreeEligible's fresh walk over `lam.stmts` structurally
    // cannot rediscover the non-escaping co-capture (#3426 condition (c))
    // that made the ancestor's answer valid — `outer` does not capture
    // itself. The ancestor's whole-function-body walk (collectCellFreeEligible's
    // #1135 widening, run once for the OUTERMOST body that actually
    // declares `x`) already proved the fact correctly for the ENTIRE
    // nesting structure; `emplace` only inserts an ABSENT key, so THIS
    // body's own fresh answer always wins for any key it DOES recompute —
    // no conflict, only filling gaps for inherited captures. Applies to
    // BOTH branches above (the expr-body branch never calls
    // collectCellFreeEligible at all, leaving the member empty after the
    // clear above — the merge is exactly as needed there too).
    //
    // Without this, `sharedCellEnvdtorFreeOwner_[x] = inner` (set correctly
    // while analyzing the ANCESTOR function's own body) is silently lost
    // the moment `outer`'s own lowerLambda clears the member for its own
    // re-scan above — and `inner`'s envdtor is generated INSIDE that exact
    // window (a nested lowerLambda call for `inner` happens while
    // `outer`'s own re-scan state, cellFreeEligibleFn_ == outer's fn, is
    // still the active one `isSharedCellEnvdtorFreeOwner` reads against),
    // so `inner` never learns it is the designated owner and `x`'s cell
    // leaks. Issue #3804's own two probes (block- and expression-bodied
    // `outer`) reproduce this precisely.
    for (const auto& [k, v] : ancestorSharedCellOwner3804)
        sharedCellEnvdtorFreeOwner_.emplace(k, v);
    // fix #4226 (§27.5 / §28.9, v0.298): merge the ancestor's
    // returnedCaptureVars_ back in for any key THIS body's own rescan (just
    // above, inside collectBoxCaptureLocals for a block body, or left empty
    // for an expression body — same "applies to BOTH branches" reasoning as
    // the #3804 merge immediately above) does not know about. A plain
    // `insert` is correct (not `emplace`-into-a-map): the set is a pure
    // membership fact ("returned by name somewhere in the enclosing chain"),
    // so a union is always sound and this lambda's own fresh discoveries
    // (if it itself directly returns the same decl) are simply ALSO in the
    // set either way.
    for (const void* k : ancestorReturnedCaptureVars4226)
        returnedCaptureVars_.insert(k);
    // fix #4240 (§27.5 / §28.9, v0.298): merge the ancestor's
    // multiCaptureLocals_ back in for any key THIS body's own rescan does
    // not know about (same "applies to BOTH branches" reasoning as the two
    // merges immediately above — the expr-body branch never touches
    // multiCaptureLocals_ at all, so the loop below is a harmless self-union
    // there), AND ERASE those same keys from THIS scan's singleCaptureLocals_.
    // A plain union alone is not enough: singleCaptureLocals_'s callers
    // (mutatedSharedCaptureCell, the two #694/#3426 sole-holder cell-free
    // gates) all read singleCaptureLocals_ directly, so the ancestor's
    // "known shared by 2+" verdict must overturn a wrong "looks single"
    // membership THIS rescan produced, not merely coexist with it in a
    // second set nobody consults. Erasing is sound in only one direction,
    // and it is the safe one: it can only REMOVE a key from
    // singleCaptureLocals_, never add one, so it can only make the
    // sole-holder gates MORE conservative (a bounded leak on the newly
    // excluded key, never an over-free) — never less.
    for (const void* k : ancestorMultiCaptureLocals4240) {
        multiCaptureLocals_.insert(k);
        singleCaptureLocals_.erase(k);
    }
    // fix #4261 (§28.3, v0.298): sync lambda body emission clobbers
    // closureReleaseEligibleFn_ (set to the lambda's fn just below), which
    // breaks lowerLocalDecl's `currentFn_ == closureReleaseEligibleFn_` guard
    // for THIS lambda's own closure cell: the check fails (outer-fn ==
    // inner-fn) and closureRelease is never registered for this lambda's cell
    // → deterministic 40 B/invocation leak (lam$2 in the issue's repro).
    // Save closureReleaseEligibleFn_ (not the set — it is restored at line
    // 3287 from the save at line 2354, which happened BEFORE lowerBody
    // populated it with the lambda's own locals).  The set is NOT saved here
    // because lowerBody's collectClosureReleaseEligible call modifies it in
    // place; restoring the pre-lowerBody Main state at line 3287 (before
    // lowerLocalDecl runs for this lambda's own cell) is what makes the guard
    // pass.  The async path mirrors this pattern (save fn only, not set).
    savedClosureRelEligFnOuter = closureReleaseEligibleFn_;
    closureReleaseEligibleFn_ = fn;
    // fix #4174 follow-up: the closure twin of the string parameter retain
    // above (line ~2479) — see retainAssignedClosureParams' own comment.
    // Paired with the closureReleaseEligibleFn_ = fn pin just above and the
    // !lam.exprBody collectClosureReleaseEligible(lam.stmts) call earlier in
    // this function; an expression body has no assignment statement, so it
    // is excluded exactly as the string sibling excludes it.
    if (!lam.exprBody && paramSyms)
        retainAssignedClosureParams(lam.stmts, *paramSyms);
    // Emit body.
    if (lam.exprBody) {
        llvm::Value* v = nullptr;
        // §15.4 (#1825): a lambda's expression body that is a collection literal
        // has no standalone type — lowerExpr returns null for a
        // CollectionLiteralExpr. The sema side now propagates the lambda's target
        // return type (a §15.4-admissible concrete target) onto the literal's
        // exprType for the typed-local / function-typed-call-arg cases, so the
        // body is typed. Materialize it here against that type — exactly like
        // the return-stmt, local-decl, and call-arg paths (LlvmBackend_Stmts.cpp
        // lowerReturnStmt, Setup.cpp lowerLocalDecl, Assign.cpp
        // materializeCollectionLiteralForArg). Empty dict literals are
        // redirected to the dict materializer by materializeHeapCollectionLiteral
        // itself, so the call is uniform.
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                lam.exprBody.get())) {
            if (sema::TypeRef* rt = ctx_.exprType(*lam.exprBody)) {
                if (!cl->isDict) {
                    // fix #4331 (§15.1 / §8.6, v0.298): an expression-bodied
                    // lambda returning an inline array (`() -> [N]T`) was never
                    // handled here — `materializeHeapCollectionLiteral` returns
                    // null for inline arrays (they are not heap collections), and
                    // `lowerExpr` returns null for `CollectionLiteralExpr`, so
                    // `v` stayed null and the lambda emitted a null/zero inline
                    // array return. Mirror the inline-array materialization logic
                    // from `lowerReturnStmt` (LlvmBackend_Stmts.cpp lines ~2139-
                    // 2190) and `lowerLocalDecl` (LlvmBackend_Setup.cpp lines
                    // ~14581-14637): build the inline array on the stack and
                    // return it by value.
                    if (rt->kind == sema::TypeRef::Kind::InlineArray) {
                        if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(
                                lowerTypeRef(rt))) {
                            llvm::Type* elemTy = aty->getElementType();
                            llvm::AllocaInst* tmp = entryAlloca(aty);
                            for (std::size_t i = 0;
                                 i < cl->elements.size(); ++i) {
                                if (!cl->elements[i]) continue;
                                llvm::Value* ev = lowerExpr(*cl->elements[i]);
                                if (!ev) {
                                    ev = lowerCollectionLiteralElementOrUnitNull(
                                        *cl->elements[i], elemTy);
                                    if (!ev) continue;
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
                                             : builder_.CreateTrunc(ev,
                                                                    elemTy);
                                }
                                // fix #4331 / §8.6: box a value-typed (record)
                                // element into its box representation before the
                                // nullable-slot wrap below — mirrors the same
                                // boxForTarget call in `lowerReturnStmt` (#4317)
                                // and `lowerLocalDecl` (#4335). No-op for a class
                                // element (already a bare pointer) or a
                                // non-Object/interface element type.
                                ev = boxForTarget(ev, rt->arrayElement,
                                                  cl->elements[i].get());
                                llvm::Value* gep = builder_.CreateGEP(
                                    aty, tmp,
                                    {builder_.getInt32(0),
                                     builder_.getInt32((int)i)});
                                // #813 / §15.1: wrap into the nullable-value
                                // slot struct `{i8, T}` for a `[N]T?` literal.
                                builder_.CreateStore(
                                    coerceToNullableSlot(elemTy, ev), gep);
                            }
                            v = builder_.CreateLoad(aty, tmp, "lam.inlarr");
                        }
                    } else {
                        v = materializeHeapCollectionLiteral(cl, rt);
                    }
                } else {
                    v = materializeHeapDictLiteral(cl, rt);
                }
            }
        }
        if (!v) v = lowerExpr(*lam.exprBody);
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
                // fix #1697 / §22.6 (v0.184) "Lambda-literal arguments": a
                // NON-nullable numeric widening at a closed function-typed
                // slot — e.g. `M(f: (int) -> long)` called as
                // `M((x) => x * 2)`, where the lambda's own inferred return
                // type (int, from the body) implicitly converts to the
                // target's return slot (long) per §22.6's admission rule
                // ("...implicitly converts to it ([§5.1]...)"). retTy above
                // was overridden to the target's LLVM type (i64) via
                // expectedFunctionType_ (the `retTy == i32()` sentinel check
                // above conflates "unresolved" with a genuinely-int body
                // type), but `v` itself was never widened to match —
                // CreateRet below then emitted a bare i32 into an
                // i64-returning function ("Function return type does not
                // match operand type of return inst"). The nullable-value
                // branch just above handles the T? case; this handles the
                // non-nullable one. Mirrors the block-body ReturnStmt fix for
                // the same rule (§5.1, #746 R41-H, LlvmBackend_Stmts.cpp)
                // that already widens a narrower numeric return value against
                // `currentFn_->getReturnType()`. coerceNumericToType is a
                // no-op when no §5.1 numeric widening applies (pointer/
                // struct/already-matching values pass through unchanged), so
                // this is safe to run unconditionally ahead of the retain
                // checks below (which only fire on pointer types).
                // fix #2925: `coerceInitializerToSlot` — §5.1's narrowing
                // direction as well as its widening one, for the same reason
                // as the method arrow-body and ReturnStmt sites.
                if (v->getType() != retTy)
                    v = coerceExprValueToSlot(
                        v, retTy, lam.exprBody.get(),
                        isUnsignedTargetExpr(*lam.exprBody));
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
    // fix #3237: restore the enclosing machine's sm*_/async*_ after the sync
    // body emit (paired with the save/clear above the `if (lam.exprBody)`).
    // fix #3505: smFrameSaveSync's destructor puts the frame back at the end of
    // this block; only the members outside the retracted frame are by hand.
    asyncHandlerSpillSlots_ = std::move(savedAsyncHandlerSpillSlotsSync);
    asyncChainedReturn_ = savedAsyncChainedReturnSync;       // fix #2747
    asyncBorrowedReturn_ = savedAsyncBorrowedReturnSync;     // fix #2747
    }

    // --- Restore emitter state ---
    expectedFunctionType_ = savedExpectedFT;  // #731
    // fix #2817: restore the enclosing body's string-refcount state (see the
    // save above). Covers BOTH branches — the sync pin just above and the resume
    // pin emitAsyncLambda performs.
    stringReleaseEligible_ = std::move(savedStringReleaseEligible);
    stringBorrowRetainEligible_ = std::move(savedStringBorrowRetainEligible);
    stringBorrowAll_ = std::move(savedStringBorrowAll);
    stringLocalCandidates_ = std::move(savedStringLocalCandidates);
    valueLikeChoiceStrReleaseEligible_ =
        std::move(savedValueLikeChoiceStrReleaseEligible);
    stringReleaseEligibleFn_ = savedStringReleaseEligibleFn;
    stringReleaseAnalyzedBody_ = savedStringReleaseBody;   // fix #2641
    stringReleaseAnalyzedExpr_ = savedStringReleaseAnalyzedExpr;  // fix #4044
    currentReturnTypeNode_ = savedCurrentReturnTypeNode;  // #2515
    currentLambdaReturnSemaTy_ = savedLambdaRetSemaTy;    // fix #2688
    lambdaParamTypeHint_ = std::move(savedLambdaParamHints);
    // fix #4813 (§14.1 / §14.5, v0.307): a hint-injected type belongs to THIS
    // specialization of the enclosing generic body (it was resolved through
    // the active substitution). `ctx_` outlives the specialization, so leaving
    // it made the next specialization of the same lambda read the previous
    // one's type (`if (!exprType) inject` never refires): wrong-typed IR or a
    // silently wrong value. Reset to "no sema type", the pre-injection state.
    for (std::size_t i = hintInjectedMark;
         i < lambdaHintInjectedExprs_.size(); ++i)
        ctx_.setExprType(lambdaHintInjectedExprs_[i], nullptr);
    lambdaHintInjectedExprs_.resize(hintInjectedMark);
    // fix #4261 (§28.3, v0.298): restore outer function's closure-release
    // eligibility (sync path; async path restores at its own site, lines 2677).
    closureReleaseEligible_ = std::move(savedClosureRelEligOuter);
    closureReleaseEligibleFn_ = savedClosureRelEligFnOuter;
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
    // fix #2886: restore the enclosing body's accessor-end block (the
    // lambda body emit nulled this above its own emission).
    inlinedAccessorEnd_ = savedInlinedAccessorEnd;
    if (savedBB) builder_.SetInsertPoint(savedBB);

    // --- Build the closure struct { fn_ptr, env_ptr, rc, envdtor, kind } ---
    // #422 / #672 / §27.5 / §28.9 / §27.7.1 (#1544): allocate a heap closure cell
    // { ptr fn, ptr env, i64 rc, ptr envdtor, i64 kind } (40 bytes). fn (off 0)
    // and env (off 8) keep their offsets; getTypeAllocSize is the single source
    // of truth for the size so the rc/envdtor/kind stores never overflow.
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
            // fix #4509 (§27.5 / §28.9, v0.306): announce this env as a holder
            // of a REFCOUNTED capture cell. Emitted HERE, at the one site that
            // stores the cell pointer into an env, so the retain and the store
            // are the same event — one retain per env, on whatever path builds
            // the env, with no way for a conditional capture path to retain a
            // cell it never stored. The matching drop is in this lambda's
            // envdtor; the frame's own drop is the `Disposal::cellRelease`
            // registered at the capture site. See CaptureEntry::retainCellInEnv.
            //
            // This is a reference to the CELL, which is a different object from
            // the cell's CONTENTS — the retain the long note below declines is
            // on the contents, and that reasoning is untouched.
            if (ce.retainCellInEnv)
                builder_.CreateCall(
                    declareC("bz_cell_retain", llvm::Type::getVoidTy(llvm_),
                             {ptrTy()}),
                    {ce.envValue});
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
    // §27.7.1 (#1544): a lambda equals only itself — tag the cell REF_IDENTITY
    // (the calloc default; set explicitly to document intent and pin the value).
    initClosureKind(closureObj, FVK_REF_IDENTITY);

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
                // fix #2706 / §27.5 / §28.9 (v0.240): suppressContentRcInEnvdtor
                // says the enclosing function WRITES this shared cell, so "the +1
                // we retained" is a lie — the retain landed on the value present
                // at CAPTURE and this load yields whatever the last write left
                // behind. N envs would each release that final value against its
                // single +1: a double free (the issue's `SEGV in
                // bz$lam$0$envdtor`). The retain at the capture site is skipped
                // under the same flag, so the two stay in agreement; the final
                // value leaks instead — the bounded, non-over-releasing side.
                if (ce.suppressContentRcInEnvdtor) {
                    // no content release: see above.
                } else if (isStringTypeRefNonNull(ce.semaTy)) {
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
                } else if (ce.freeCellInEnvdtor &&
                           isDictEntryStringKeyTypeRef(ce.semaTy)) {
                    // fix #4127 (§27.5/§28.9, stdlib §4.8, v0.295): a
                    // string-keyed `DictionaryEntry<string,V>` local (bound
                    // directly to `d.First()`/`.Last()`/`.Single()`/
                    // `.ElementAt(i)`, per isFreshDictEntryKeyReleaseTerminalLocal
                    // / #3720) captured by the sole, definitely-escaping
                    // closure this envdtor belongs to (the SAME
                    // freeCellInEnvdtor proof that makes freeing the cell
                    // below safe: exactly one holder, and that holder
                    // outlives the frame — #694). The local's own scope-end
                    // `dictEntryKeyRelease` disposal was never registered for
                    // it (StringEscapeWalker treats ANY capture as an escape,
                    // per that disposal's own eligibility walk), so the
                    // rebuilt Key's single producer +1 is otherwise never
                    // discharged — exactly #4127's leak. The cell holds the
                    // `{ptr Key, V Value}` struct BY VALUE; Key is field 0,
                    // so loading a `ptr` directly off the cell's base address
                    // (no GEP needed — field 0 is always at offset 0) reads
                    // it, identical in shape to the plain-string-capture
                    // branch above. `entry.Value` is never touched here: a
                    // Dictionary<K,V> keeps ownership of its values (stdlib
                    // §4.8) — `Value` is always a borrow, never released.
                    // NOT extended to a for-loop variable's DictionaryEntry
                    // capture (`entry` in `for var entry in d`): `!isForVar`
                    // is a hard precondition of `freeCellInEnvdtor` itself
                    // (a for-var's cell is fresh per iteration, so no single
                    // envdtor could be its one reclaim site) — that shape is
                    // #4129/#4161's territory, unaffected by this branch. No
                    // retain-balancing concern either: singleCaptureLocals_
                    // guarantees no OTHER capture of this local exists to
                    // share the +1 with.
                    llvm::Value* key =
                        builder_.CreateLoad(ptrTy(), cell, "dictek.envdtor");
                    emitStringRelease(key);
                } else if (ce.freeCellInEnvdtor &&
                           isDrainedEntryTypeRef(ce.semaTy)) {
                    // fix #4135 (§27.5/§28.9, stdlib §4.9, v0.295): a
                    // DrainedEntry<K,V> for-loop variable (from `for var entry
                    // in d.Drain()`) captured by the sole, definitely-escaping
                    // closure (freeCellInEnvdtor, the same proof that makes
                    // freeing the cell below safe: singleCaptureLocals_ ∧
                    // definitelyEscapingLambdas_ — #694). The local's own
                    // scope-end DrainedEntry disposal was never registered for it
                    // (the eligibility walk treats ANY capture as an escape), so
                    // the owned Key + owned Value are both never discharged —
                    // exactly #4135's leak. The cell holds the {K, V*} struct
                    // BY VALUE; release both fields here, mirroring the
                    // scope-end drainedEntry disposal in LlvmBackend_Disposal.cpp.
                    const sema::TypeRef* entryTy = ce.semaTy;
                    const sema::TypeRef* kTy = entryTy->genericArgs[0];
                    const sema::TypeRef* vTy = entryTy->genericArgs[1];
                    llvm::Type* kLlvm = lowerTypeRef(kTy);
                    llvm::Type* vLlvm = lowerTypeRef(vTy);
                    if (kLlvm && vLlvm) {
                        llvm::StructType* entStTy =
                            llvm::StructType::get(llvm_, {kLlvm, vLlvm});
                        // Step 1: dispose the owned V class at field[1] (V*
                        // — V is always a class per stdlib §4.9's
                        // OwningDictionary<K,V> constraint). emitDisposeAndReclaim
                        // is null-safe so no null check needed.
                        if (vLlvm->isPointerTy()) {
                            llvm::Value* valFldPtr =
                                builder_.CreateStructGEP(entStTy, cell, 1, "dde.val.ptr");
                            llvm::Value* valPtr =
                                builder_.CreateLoad(ptrTy(), valFldPtr, "dde.val");
                            emitDisposeAndReclaim(valPtr);
                        }
                        // Step 2: if K is string, release the owned key at field[0].
                        if (kTy->kind == sema::TypeRef::Kind::Primitive &&
                            kTy->primitive == sema::PrimitiveType::String) {
                            llvm::Value* keyFldPtr =
                                builder_.CreateStructGEP(entStTy, cell, 0, "dde.key.ptr");
                            llvm::Value* keyPtr =
                                builder_.CreateLoad(ptrTy(), keyFldPtr, "dde.key");
                            emitStringRelease(keyPtr);
                        }
                    }
                }
                // All other capture types (value, class borrow): no action.
                // §27.5 / #694: free the 8-byte capture CELL itself when this is
                // the sole env of a DEFINITELY-escaping capture (freeCellInEnvdtor).
                // Runs AFTER the content release above (which reads through the
                // cell). The scope-end boxedCellFree did not fire (the closure
                // escaped), and this is the single holder, so this is the cell's
                // only free — no double-free; the defining scope is gone before the
                // escaped closure dies, so no later access through the cell — no UAF.
                // fix #4509 (§27.5 / §28.9, v0.306): drop THIS env's counted
                // reference to a refcounted capture cell. Runs after the
                // content-release branches above, which read through the cell,
                // and is the mirror of the `bz_cell_retain` at the env's
                // capture-store. The cell is freed here only if this env is the
                // last holder — if the frame has already let go and no sibling
                // env remains. When the frame is still live (the ordinary case
                // for a subscription disposed inside the declaring scope) the
                // count merely falls and the frame's own later read through the
                // cell is safe; when the env OUTLIVES the frame the order is
                // reversed and this is the free. Neither order needs a static
                // proof, which is the whole point.
                //
                // Mutually exclusive with `freeCellInEnvdtor` below: that flag
                // arms only on #694/#3426's proven-sole-holder routes, which
                // are preconditions this classification excludes.
                if (ce.retainCellInEnv)
                    builder_.CreateCall(
                        declareC("bz_cell_release", llvm::Type::getVoidTy(llvm_),
                                 {ptrTy()}),
                        {cell});
                if (ce.freeCellInEnvdtor) {
                    // fix #4210 (§27.5 / §28.9, v0.298): store the freed cell
                    // pointer to the module-level global so boxLocalSlot detects
                    // the stale cellVars_ entry and allocates fresh on the next
                    // loop iteration.  The `cell` value here is loaded from the
                    // env struct (envArg) within this envdtor function, so it is
                    // a valid SSA value in this scope.
                    llvm::GlobalVariable* freedGlobal = getFreedSlotGlobal();
                    builder_.CreateStore(cell, freedGlobal);
                    builder_.CreateCall(freeFn(), {cell});
                }
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
    // `expr with name := v, ... end` — §9.5 (v0.188): a non-destructive
    // RECORD copy — the source record is unchanged. The source is a
    // first-class struct value, so spill it to a fresh stack slot (the
    // copy), overwrite the named fields through that pointer, and reload
    // the result by value. Using the struct value directly as a GEP base is
    // malformed (and hung the compiler — #254).
    //
    // Before v0.188 this function also lowered the CLASS "object initializer
    // block" reading of `with` (former §22.11, now a tombstone): the source
    // was a freshly-`new`-constructed instance, mutated in place through its
    // setters and returned by the same pointer. Proposal 0037 (spec v0.188)
    // retired that reading — a class-typed `with` source is now a compile
    // error (§9.5, rejected in ExprTypecheck_Exprs.cpp before codegen ever
    // runs) — so the class-only machinery this function used to carry
    // (custom-setter dispatch, the #747 R43 unattached-owned disposal
    // landing pad for a setter that throws mid-sequence, the #429 string-
    // field retain/release on a class-instance store) is unreachable and
    // retired with it (#1735).
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
    if (t->kind != ast::TypeDeclKind::Record) {
        // Unreachable for a class/interface source: §9.5 rejects it at sema
        // before codegen runs (#1735). The only other non-record kind an
        // exprType can carry here is Enum, which §9.5 also requires to be
        // record-typed but which the current sema gate does not yet enforce
        // (a separate, pre-existing gap — filed as a follow-up, not part of
        // #1735: an enum has no instance fields or auto-properties to serve
        // as a `with` target at all, §10.5, so this path was never able to
        // do anything useful for one). Report an internal error rather than
        // GEP through class-shaped storage a bare enum value doesn't have.
        ctx_.diagnostics().report(
            Severity::Error, we.source->span.begin,
            "internal: `with` on a non-record source reached codegen "
            "unexpectedly — should have been rejected at sema (§9.5) — "
            "#1735");
        return nullptr;
    }
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
    // classStruct routes a record to recordStruct (no vtable) — the right
    // layout here, since `t` is now known to be a record.
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
        rt && rt->nullable && rt->kind == sema::TypeRef::Kind::Named &&
        rt->declaration && rt->declaration->kind == ast::TypeDeclKind::Record &&
        !src->getType()->isPointerTy();
    llvm::Value* nullableTag = nullptr;
    if (srcNullableRecord) {
        nullableTag = builder_.CreateExtractValue(src, {0u}, "with.nul.tag");
        src = builder_.CreateExtractValue(src, {1u}, "with.nul.payload");
    }
    // The GEP base: a spilled copy of the source record.
    llvm::Value* base = entryAlloca(st);
    // #575 / §9.5 / §22.3: when `this with { … }` is used inside a record
    // instance method, lowerExpr(this) returns currentThis_ which is a
    // POINTER to the record (CG29 — record methods receive `this` as ptr,
    // §9.4). A regular non-this record source is already a struct VALUE.
    // Load the struct from the pointer before spilling to the copy alloca;
    // storing a ptr into a struct-typed alloca would corrupt the copy and
    // produce a "return type does not match" IR verification failure.
    {
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
        // fix #4346 / §15.4 / §9.5: a `with`-replacement value that is a
        // collection literal `[e1, …]` targeting a HEAP-COLLECTION field
        // (`Array<T>`, `Dictionary<K,V>`, etc.). lowerExpr returns null for
        // CollectionLiteralExpr, so the plain `lowerExpr` + `if (!v) continue`
        // below hit the #1386 backstop and ICEd. Mirror the LocalDecl /
        // FieldDecl / ReturnStmt / call-arg paths: materialize a fresh
        // `bz_array_t*` (or `bz_dict_t*`) and store the pointer into the
        // field slot. The source record is unmodified and still owns its own
        // collection — the new record owns the freshly-materialized one —
        // so no dispose-old is needed here (the new slot's ownership is
        // established when we store the new pointer; the source's collection
        // outlives the with-expression because nothing has touched it).
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                a.value.get())) {
            const sema::TypeRef* fldTy = recordMemberTypeResolved(m);
            if (fldTy) {
                if (llvm::Value* heapArr =
                        materializeCollectionLiteralForArg(cl, fldTy)) {
                    llvm::Value* fslot =
                        builder_.CreateStructGEP(st, base, idxIt->second);
                    // fix #4350 / §28.9 / §21.1: the freshly-materialized heap
                    // collection is stored into the result record's field. The
                    // result record is a value type returned by `lowerWith` and
                    // has no Dispose method (records are borrow-only for heap
                    // collection fields — §9.7 "no disposer"); without this
                    // registration the bz_array_t (and its backing for a
                    // Dictionary) leaks. Register the disposal at the current
                    // scope: create a shadow alloca holding the pointer so the
                    // disposeStack_ entry is valid at scope-end emission.
                    llvm::AllocaInst* shadow = entryAlloca(heapArr->getType());
                    builder_.CreateStore(heapArr, shadow);
                    if (!disposeStack_.empty())
                        disposeStack_.back().push_back(
                            buildCollectionTempDisposal(shadow, fldTy));
                    builder_.CreateStore(heapArr, fslot);
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
        // A RECORD `with`-copy is a value type whose string fields are
        // borrows (§9) and has no disposer — a plain store (no retain).
        // (Before v0.188 a CLASS object-initializer store into a string
        // field/auto-property took an independent +1 here, released on the
        // instance's disposal, #429 / §4.6 / §28.9 — retired with the class
        // form, #1735.)
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
    // Return the modified copy by value.
    llvm::Value* result = builder_.CreateLoad(st, base);
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

namespace {

// fix #3583 (§22.8 / §22.10 / §12.8, v0.286): §22.8's own predicate — "a
// constructor that chains via `this(...)` **skips steps 0 and 2**". The `new`
// site asks it to decide whether to run step 0 there or leave it to the
// chained-to primary, which is where §22.8 puts it for such a constructor. A
// null `cs` (no explicit constructor bound — the call goes straight to the
// primary) answers false, because the primary constructor is precisely the one
// that does NOT skip step 0. File-local rather than an `Emitter` member: it
// reads only the AST, and both of its callers are in this file — the `new`
// site (#3583) and, since fix #3562, the explicit `base(...)`-chain arm of
// `prePopulateBasePositionalFields`, which asks the identical question about
// the BASE constructor the chain call binds to.
bool ctorChainsViaThis(const sema::ConstructorSymbol* cs) {
    if (!cs || cs->isStatic) return false;
    // The chain FORM of a constructor body has no symbol-side channel: nothing
    // on `ConstructorSymbol` records "chains via `this(...)`" — only the body's
    // first statement does, and an imported constructor has no body to read. So
    // the cast below answers false for an import. That falseness is inert at
    // both call sites, but for DIFFERENT reasons, and conflating them hides a
    // live gap:
    //   * at the `new` site (#3583) the imported arms gate on
    //     `!hasAnyConstructor(t)` before reaching here, and an imported primary
    //     constructor plus a convenience constructor is non-functional end to
    //     end regardless (measured identical before and after fix #3583);
    //   * at the `base(...)`-chain site (#3562) there is NO such gate. False
    //     there means the blind positional pre-population still runs for an
    //     imported base whose `base(...)` binds a convenience constructor —
    //     i.e. the cross-module face of #3562 is UNFIXED, and this answer
    //     preserves that behaviour rather than making it inert.
    // If the chain form is ever carried across the wire, this read must be
    // routed through the new channel rather than kept, and the `base(...)`
    // site's cross-module face closes with it.
    // SEAM-DEBT: #3625 no symbol-side channel for a constructor body's chain form
    auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
    if (!cd || cd->body.empty() || !cd->body[0]) return false;
    bool isThis = false;
    return ast::chainCallOfStmt(*cd->body[0], isThis) && isThis;
}

}  // namespace

// fix #3777 (§22.5 "Optional parameters and named arguments" / §22.10 "primary
// constructors" / §22.8 step 0, v0.286): the ONE materialization of a §22.10
// positional parameter's §22.5 default. Extracted verbatim from
// `storePrimaryCtorPositionalArgs`, whose two channels this used to be, because
// `prePopulateBasePositionalFields` now has to fill the same omitted slot for a
// BASE class and a second spelling of "what value does an omitted positional
// parameter take" is the defect class CLAUDE.md §6 names.
//
// LOCAL (`posParam->defaultValue`): lowered under the OUTER substitution but
// with `currentThis_ = obj`, because the default may read EARLIER primary-ctor
// parameters on the SAME list (§22.5 "Permitted default-value expressions") and
// those live in `obj`'s captured field slots, not in the enclosing scope. Both
// callers store slot `i` before evaluating slot `i+1`'s default, which is the
// left-to-right order that rule relies on.
//
// IMPORTED (`posParam->defaultText`): folded through
// `lowerImportedPositionalDefault` — #3202's REC_POSPARAM_DEFAULT text through
// the same `foldImportedConstText` every other imported constant read uses.
//
// Returns null when the parameter declares no default at all, and when the
// default failed to lower (`lowerImportedPositionalDefault` has already
// reported in that case); both callers then leave the slot alone.
llvm::Value* Emitter::positionalParamDefaultValue(
        const sema::FieldSymbol& fs,
        llvm::Value* obj,
        const std::unordered_map<std::string_view, llvm::Type*>& outerSubst,
        const std::unordered_map<std::string_view, const sema::TypeRef*>&
            outerSemaTy) {
    if (!fs.posParam) return nullptr;
    if (fs.posParam->defaultValue) {
        // local — evaluate under the outer substitution, with
        // `currentThis_ = obj` so earlier primary-ctor params resolve through
        // their captured slots.
        llvm::Value* savedThisForArg = currentThis_;
        currentThis_ = obj;
        auto posSubst = genericSubst_;
        auto posSemaTy = genericSubstSemaTy_;
        genericSubst_ = outerSubst;
        genericSubstSemaTy_ = outerSemaTy;
        llvm::Value* defaultV = lowerExpr(*fs.posParam->defaultValue);
        genericSubst_ = std::move(posSubst);
        genericSubstSemaTy_ = std::move(posSemaTy);
        currentThis_ = savedThisForArg;
        return defaultV;
    }
    if (!fs.posParam->defaultText.empty())
        return lowerImportedPositionalDefault(fs);  // imported — textual
    return nullptr;
}


// §22.8 step 0 / §22.10 (fix #3162) — see LlvmBackend_impl.hpp for the
// contract and for why this is a method rather than two copies.
void Emitter::storePrimaryCtorPositionalArgs(
        const sema::TypeSymbol* t,
        llvm::Value* obj,
        llvm::StructType* st,
        const std::vector<ast::CallArgument>& args,
        const std::vector<int>* binding,
        const std::unordered_map<std::string_view, llvm::Type*>& outerSubst,
        const std::unordered_map<std::string_view, const sema::TypeRef*>&
            outerSemaTy,
        llvm::Value* thisForArgs) {
    // fix #3141 (family F3 of umbrella #3152, §22.8 step 0): the primary-
    // constructor parameter list is the indexed symbol-side view
    // (`recordPositionalFieldSymbols`), which is the same list `td->
    // positionalFields` names — a local positional FieldSymbol's `posParam`
    // points into it — reached so that the pairing of argument i with field
    // i does not depend on an `ast::TypeDecl`. It also replaces the O(n^2)
    // `posParam == &pf` scan over `members` this used to do per argument.
    // The flag is checked FIRST so the vector is built only for a
    // primary-constructor class, not on every `new`.
    //
    // fix #4722 (a) (§9.1 / §9.3 / §12.8 / §22.8 step 0, v0.307): a RECORD
    // with positional fields is admitted too. Its §9.1 primary constructor
    // is reached this way only from `emitChainCall`'s primary arm — a body
    // constructor's `this(...)` chain, which §9.3 keeps "governed by §12.8
    // unchanged". Every `lowerNew` caller of this helper sits on the class
    // path, after the record branch has already returned through
    // `buildRecordValue`, so no `new R(...)` site reaches it. Before this the
    // gate returned early for a record and the chain stored NOTHING: the
    // object kept its zero fill and printed `R(X: 0, Y: 0)`.
    if ((t->kind != ast::TypeDeclKind::Class &&
         t->kind != ast::TypeDeclKind::Record) ||
        !t->hasPositionalFields)
        return;
    const std::vector<const sema::Symbol*> posFields =
        sema::recordPositionalFieldSymbols(t);
    if (!posFields.empty()) {
      // fix #3202 (§22.10 "primary constructors" / §22.5 "Optional parameters
      // and named arguments" / §22.8 step 0, v0.254): a §22.10 primary
      // parameter may carry a DEFAULT (`class DefBox(A: int, B: int := 9)`)
      // and an under-supplied call must fill the trailing defaulted slots
      // from the parameter's `:= Expr` rather than leaving the object
      // calloc-zero. The OLD `ne.args.size() == posFields.size()` gate
      // skipped the entire store loop whenever the call was short, so a
      // `new DefBox(1)` left the WHOLE OBJECT zero — not just the unbound
      // slot — and `Total()` read 0 instead of 10. The LOCAL side
      // (`fs->posParam->defaultValue` Expr*) and the IMPORTED side
      // (`fs->posParam->defaultText` string, the textualised default the
      // encoder shipped via #3202's REC_POSPARAM_DEFAULT) are the two
      // channels through which a default reaches this site.
      // fix #3196 (§22.5 "Optional parameters and named arguments" /
      // §22.10 "primary constructors" / §22.8 step 0, v0.254): read the
      // §22.5 BINDING sema published on the `SemaContext` side table via
      // `checkPositionalNewArgsFromSymbols` →
      // `SemaContext::setNewExprPositionalBinding`. The pre-#3196 path
      // paired `ne.args[pi]` with field `pi` positionally, which silently
      // zeroed every slot on a NAMED call (`new PBox(W := 4, V := 3)`
      // would store `4` into V's slot and `3` into W's slot) — the
      // silent-wrong-object outcome #3141 declined rather than ship, and
      // was also the silent-wrong OBJECT on the LOCAL side (#3163). #3196
      // closes both halves by reading the same `fieldToArg` sema resolved,
      // so there is exactly one §22.5 binding body in the compiler.
      //
      // The binding is published by:
      //   - #3162's IMPORTED-class-with-primary-ctor arm (no explicit ctor,
      //     non-empty args) — runs `checkPositionalNewArgsFromSymbols`.
      //   - #3196's twin: LOCAL class-with-primary-ctor + no explicit ctor
      //     + non-empty args (was previously deferred outright by
      //     `if (!ctx_.isImported(t)) return;` at sema line ~10824, so the
      //     local twin was the silent-wrong-object side; that deferral is
      //     gone now too).
      //   - the LOCAL-record-with-positional-fields check
      //     `checkArgsAgainstParams`, and the IMPORTED-record twin
      //     `checkPositionalNewArgsFromSymbols` (records don't call THIS
      //     helper, so the publication is a benign side-effect for them).
      //
      // When the binding IS NOT published (legacy / paths sema has not
      // reached — among them, #3163's combined "primary ctor + explicit
      // ctor" deferral at sema's `checkNewExprArgs` lines 17215-17248,
      // whose `if (hasExplicitCtor) return;` short-circuits before
      // `checkPositionalNewArgsFromSymbols` can publish), fall back to
      // position-i pairing when all positional, or build the §22.5
      // binding on demand from the call's named arguments when at least
      // one is named. The named-arm build path keeps the fix self-
      // contained at THIS lowering site: it does not require sema to
      // publish, and the binder body mirrors `bindArgsToParams`'s
      // named-vs-positional rule (§22.5 "Optional parameters and named
      // arguments" — positional precedes named, names match primary
      // parameters, no duplicate binding, every required slot filled or
      // defaulted).
      //
      // fix #5107 (§22.10 "primary constructors" / §22.5 "Optional
      // parameters and named arguments" / §9.3 "Combined form";
      // spec v0.307): a LOCAL class combined-form class
      // (`class C(x: int, z: int)` + `constructor(y: int) this(y, 0);`)
      // with `new C(x := 81, z := 0)` reached this helper with
      // `binding == nullptr` (sema's deferral returned without
      // publishing it — dc 2026-09-28 ruled the previous lane's sema
      // re-publication fix at `reportPrimaryBodyCtorAmbiguity` lines
      // 16992-17003 unreachable for the non-ambiguous case: the helper
      // returns false on `cands.size() < 2` BEFORE the publish call).
      // The pre-fix posture — `return` on a named-without-binding
      // argument — left every primary field at calloc-zero: the named
      // `x := 81, z := 0` was dropped, the call's store skipped, and
      // `c.X` read 0. Building the binding here, by name match against
      // `recordPositionalFieldSymbols(t)`'s `posParam->name`, restores
      // the spec-named-argument store without re-deciding the ctor —
      // `ctorForArgs` already chose what to call; this helper only
      // needs the per-slot argument index.
      const std::vector<int>* bindingPtr = binding;
      std::vector<int> computedBinding;
      if (!bindingPtr) {
          bool hasNamed = false;
          for (const auto& a : args)
              if (!a.name.empty()) { hasNamed = true; break; }
          if (hasNamed) {
              // Build a §22.5 binding from the call's named args +
              // `posFields`' declared order. The `posParam` field
              // carries the parameter name on both the LOCAL side (the
              // class's actual `ast::Param` declared in the class header)
              // and the IMPORTED side (the synthetic `ast::Param` #2516
              // attaches so the positional/body distinction survives
              // without an AST — see `FieldSymbol::posParam` at
              // Symbol.hpp:805). Match on `std::string_view` identity,
              // not a string allocation — the call-side `CallArgument::name`
              // is also a `string_view` of source text (Node.hpp:95).
              computedBinding.assign(posFields.size(), -1);
              bool viable = true;
              std::unordered_set<std::string_view> seenNames;
              std::size_t posSlot = 0;  // next positional binding slot
              bool sawNamed = false;
              for (std::size_t ai = 0; ai < args.size(); ++ai) {
                  const auto& a = args[ai];
                  if (a.name.empty()) {
                      // §22.5: a positional argument may not follow a
                      // named one, and binds the NEXT unfilled field in
                      // declaration order. Mirror `bindArgsToParams`'s
                      // loop exactly so codegen cannot drift from sema
                      // (`bindArgsToParams` at ExprTypecheck_Calls.cpp
                      // line 14518-14553).
                      if (sawNamed || posSlot >= posFields.size()) {
                          viable = false; break;
                      }
                      computedBinding[posSlot] = static_cast<int>(ai);
                      ++posSlot;
                      continue;
                  }
                  sawNamed = true;
                  std::size_t idx = posFields.size();
                  for (std::size_t fi = 0; fi < posFields.size(); ++fi) {
                      auto* fs = static_cast<const sema::FieldSymbol*>(
                          posFields[fi]);
                      if (fs && fs->posParam &&
                          fs->posParam->name == a.name) {
                          idx = fi; break;
                      }
                  }
                  if (idx >= posFields.size() ||
                      !seenNames.insert(a.name).second ||
                      computedBinding[idx] != -1) {
                      viable = false; break;
                  }
                  computedBinding[idx] = static_cast<int>(ai);
              }
              // Every required (non-defaulted) slot must be filled; a
              // defaulted slot's omission is OK because the per-field
              // loop below fills it via `positionalParamDefaultValue`.
              if (viable) {
                  for (std::size_t pi = 0; pi < posFields.size(); ++pi) {
                      if (computedBinding[pi] != -1) continue;
                      auto* fs = static_cast<const sema::FieldSymbol*>(
                          posFields[pi]);
                      if (fs && fs->posParam &&
                          (fs->posParam->defaultValue ||
                           !fs->posParam->defaultText.empty())) {
                          continue;
                      }
                      viable = false;
                      break;
                  }
              }
              if (viable) {
                  bindingPtr = &computedBinding;
              } else {
                  // The binding we COULD build does not match §22.5 —
                  // an unknown name, a duplicate, a positional-after-
                  // named, a required slot left empty. Sema would have
                  // rejected this call above us; if no binding is
                  // available, treat it conservatively as the pre-
                  // #5107 fallback: every field calloc-zero, no
                  // codegen crash, no false store. Loud rejection is
                  // sema's job (this fix does not change sema).
                  return;
              }
          } else if (args.size() > posFields.size()) {
              return;  // positional-oversupplied — every field stays
                       // calloc-zero, the pre-#3196 posture.
          }
      }
      for (std::size_t pi = 0; pi < posFields.size(); ++pi) {
              {  // scope for the per-field bindings below
                  auto* fs = const_cast<sema::FieldSymbol*>(
                      static_cast<const sema::FieldSymbol*>(posFields[pi]));
                  auto idxIt = fieldIndex_.find(fs);
                  if (idxIt == fieldIndex_.end()) continue;
                  // The argument index this slot binds to:
    //   - From the §22.5 binding sema published, when present
    //     (`bindingPtr != null`): `argIdx` is the call-site argument index
    //     that names this slot, or -1 for an OMITTED slot (a defaulted
    //     parameter the call left blank; the next branch fills it).
    //   - From the position-i legacy pairing (no binding published):
    //     `argIdx == pi` for the first `ne.args.size()` fields, and -1
    //     for the trailing slots the call did not supply.
    // The other arms of the per-field logic use `argIdx` rather than
    // `pi` to find the value to lower, so a named call (`new PBox(W := 4,
    // V := 3)` → `argIdx[0] = 1, argIdx[1] = 0`) lowers `ne.args[1].value`
    // (which is `4`) for field 0 and `ne.args[0].value` (which is `3`) for
    // field 1 — the same mapping sema accepted. The pre-#3196 positional
    // pairing `ne.args[pi]` swapped V and W, leaving both stored wrong.
    int argIdx;
    if (bindingPtr) {
        argIdx = (pi < bindingPtr->size()) ? (*bindingPtr)[pi] : -1;
    } else {
        argIdx = (pi < args.size()) ? static_cast<int>(pi) : -1;
    }
                  // The argument expression to lower: supplied (by the §22.5
                  // binding) when the slot has a bound arg, the parameter's
                  // `:= Expr` default otherwise. `ne.args[argIdx].value`
                  // is null both when the slot is omitted AND when it is
                  // `out _`-shaped — but the latter never reaches this site
                  // (a positional primary-ctor parameter admits plain and
                  // `owned` only, §22.10, so no `out` slot can bind here).
                  // A `null` value at this line therefore means
                  // "omitted, fill from default".
                  const ast::Expr* argExpr = nullptr;
                  llvm::Value* defaultV = nullptr;
                  if (argIdx >= 0 &&
                      static_cast<std::size_t>(argIdx) < args.size() &&
                      args[argIdx].value) {
                      argExpr = args[argIdx].value.get();
                  } else if (fs->posParam &&
                             (fs->posParam->defaultValue ||
                              !fs->posParam->defaultText.empty())) {
                      // §22.5 "Defaults and modifiers" + §22.10: a defaulted
                      // primary-ctor parameter. LOCAL: lower the AST
                      // `defaultValue` under the OUTER substitution
                      // (mirroring the supplied-arg arm's #597 bracket
                      // just below), but with `currentThis_ = obj` rather
                      // than `thisForArgs` — the default may read EARLIER
                      // primary-ctor parameters on the SAME list (§22.5
                      // "Permitted default-value expressions"), and those
                      // parameters live in `obj`'s captured field slots,
                      // not in the enclosing scope. The supplied-arg arm
                      // uses `thisForArgs` because the supplied arg is an
                      // enclosing-scope expression that does NOT read
                      // `t`'s own primary-ctor params (#597); the default
                      // IS a function of `t`'s own params, so `obj` is
                      // the right `this`. (For a non-captured earlier
                      // parameter the IdentifierExpr for the param's
                      // name still routes through `implicitFieldAddr` —
                      // the capture-iff-used rule §22.10 sets
                      // `posParamBodyRef`, but #2325's capture
                      // enumeration also captures a parameter used in any
                      // default of a later parameter, since that default
                      // evaluates against the object.)
                      //
                      // The order-of-evaluation property this bracket
                      // relies on is the loop's left-to-right: `pi = 0`
                      // stores its slot BEFORE `pi = 1` evaluates its
                      // default, so reading `A` (a primary-ctor param
                      // stored at `pi = 0`) inside the default for `B`
                      // (at `pi = 1`) sees the just-stored value, not
                      // the calloc zero. Mirrors §22.5 "Defaults and
                      // modifiers" / §22.8 step 0 sequencing.
                      //
                      // IMPORTED: the textual default folds through the
                      // same `foldImportedConstText` machinery
                      // `importedDefaultArgValue` uses for method
                      // parameters — the imported positional-parameter
                      // half of #3213's path.
                      // `lowerImportedPositionalDefault` materializes the
                      // constant on demand.
                      // fix #3777 (§22.5 / §22.10 / §22.8 step 0, v0.286):
                      // the two channels are now `positionalParamDefaultValue`
                      // — the same body, stated once, so that the BASE-level
                      // pre-population (`prePopulateBasePositionalFields`,
                      // below) fills an omitted defaulted positional slot by
                      // the identical rule instead of leaving it calloc-zero.
                      defaultV = positionalParamDefaultValue(*fs, obj,
                                                             outerSubst,
                                                             outerSemaTy);
                      if (!defaultV) continue;
                  } else {
                      // No arg, no default — leave the slot calloc-zero
                      // (the original behavior). This is unreachable for a
                      // well-formed program whose args list has been
                      // verified by `checkNewExprArgs` /
                      // `checkPositionalNewArgsFromSymbols` (§22.5 arity),
                      // but the explicit skip keeps the row silent rather
                      // than SIGSEGVing on an unsound GEP — same posture
                      // as the pre-fix `if (!ne.args[pi].value) continue;`
                      // line this replaces (the bound arm now reads
                      // `ne.args[argIdx].value` per the §22.5 binding).
                      continue;
                  }
                  // issue #597: a positional primary-ctor argument is an
                  // enclosing-scope expression — lower it under the outer
                  // substitution, then restore the new type's subst for
                  // the field-type coercion + store below.
                  // #2367: likewise for `currentThis_` — #597 fixed the
                  // generic-substitution half of "enclosing-scope
                  // expression" but left `currentThis_ = obj` (set above
                  // for the DIFFERENT purpose of resolving `t`'s OWN
                  // implicit-field reads, e.g. a chained base-ctor arg
                  // that names one of `t`'s own primary-ctor params) in
                  // effect across this evaluation too. An arg expression
                  // that is itself an implicit-field read of the ENCLOSING
                  // scope (e.g. `new Leaf(first)` as another class's own
                  // field initializer, where `first` is the enclosing
                  // class's primary-ctor param) resolved that read via
                  // implicitFieldAddr against `obj` — the new, not-yet-
                  // initialized `t` instance — instead of the enclosing
                  // object, reading through a pointer sized/typed for `t`
                  // as if it had the enclosing class's layout (a wrong-
                  // object GEP; heap-buffer-overflow when `t`'s instance
                  // is smaller). Restore the enclosing `this`
                  // (`thisForArgs`) for the evaluation only, mirroring
                  // the explicit-ctor arg path below (case (b) in its
                  // comment) which already does this correctly. The
                  // imported default arm (the `defaultV` branch above)
                  // never needs this bracket — its value is already a
                  // constant produced under the enclosing scope's
                  // substitution by `lowerImportedPositionalDefault`.
                  llvm::Value* v = nullptr;
                  if (argExpr) {
                      llvm::Value* savedThisForArg = currentThis_;
                      currentThis_ = thisForArgs;
                      auto posSubst = genericSubst_;
                      auto posSemaTy = genericSubstSemaTy_;
                      genericSubst_ = outerSubst;
                      genericSubstSemaTy_ = outerSemaTy;
                      v = lowerExpr(*argExpr);
                      genericSubst_ = std::move(posSubst);
                      genericSubstSemaTy_ = std::move(posSemaTy);
                      currentThis_ = savedThisForArg;
                      // fix #3502 (§4.7 "The unit type" / §22.10 / §11.1,
                      // v0.283): a unit-typed argument has NO lowered value —
                      // `()` lowers to LLVM `void`, so `lowerExpr` returns null
                      // — and this slot is a real FIELD with a real storage
                      // type, so `continue`ing left the `()?` field at its
                      // calloc zero, which IS `null`. §4.7 makes `()?` "an
                      // optional with two values, `null` and `()`", so
                      // `new B(C.Nothing())` and `new B(null)` both reading
                      // absent is a silent wrong value, not a missing feature.
                      // Routed through the SAME `unitValueForTarget` predicate
                      // the call-argument / local-decl / assignment / return
                      // positions use (#2915/#3095/#3421), with the target
                      // supplied by `recordMemberTypeResolved` — the existing
                      // authority for "the declared type of a storage member,
                      // resolved through the active substitution", which is the
                      // one already installed here (`posSubst`, the new type's
                      // own). A no-op for every non-unit source and every
                      // non-unit field, so the `continue` below still covers
                      // the shapes it was written for.
                      if (!v)
                          v = unitValueForTarget(
                              v, recordMemberTypeResolved(fs), argExpr);
                      if (!v) continue;
                  } else {
                      v = defaultV;
                  }
                  llvm::Type* fieldTy =
                      st->getElementType(idxIt->second);
                  // fix #4722 (a) (§9.1 / §23.1 / §22.8 step 0, v0.307): a
                  // NULLABLE VALUE positional slot (`X: int?`) stores the
                  // tagged `{i8 hasValue; T value}` struct, never a bare T —
                  // the same wrap `buildRecordValue`'s positional path does.
                  // Without it a bare `i32 5` was stored over the tag and the
                  // field read back as `0` (measured for a record chain to
                  // its primary, and for the class twin `new C(5, 3)` over
                  // `class C(x: int?, y: long)`).
                  //
                  // fix #5037 (§5.1 / §9.1, v0.307): the payload width/kind
                  // conversion is `wrapNullablePositionalPayload`'s job now —
                  // see its declaration comment for why the previous bare
                  // `CreateSExtOrTrunc` here sign-extended an unsigned
                  // narrower source and had no int→float arm.
                  v = wrapNullablePositionalPayload(
                      v, recordMemberTypeResolved(fs), argExpr);
                  v = coerceNumericToType(
                      v, fieldTy,
                      argExpr ? isUnsignedTargetExpr(*argExpr) : false);
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
                  //
                  // A default value is a fresh producer each call (§22.5
                  // "Evaluation timing" / §22.5 "Permitted default-value
                  // expressions"): the expression is re-evaluated at every
                  // omission, so the slot holds an INDEPENDENT reference
                  // (a literal `:= "x"` materializes a fresh string each
                  // time). For a STRING default the +1 is therefore
                  // correct via the same rule as a fresh `+1` producer
                  // argument (`isPlusOneStringProducer` returns true for a
                  // string literal — `lowerExpr` of a `string` literal
                  // already returns a +1 reference). Passing
                  // `argExpr=nullptr` here is exactly the "fresh default
                  // producer" path, so `isPlusOneStringProducer(nullptr)`
                  // is the right test — implemented inline as `!argExpr`
                  // for the imported default arm and the local literal
                  // path; the non-literal local-default arm (a default
                  // expression that LOWERs to a non-+1 string, e.g. a
                  // field read) retains the same rule by the same
                  // expression-pointer identity, which is what
                  // `isPlusOneStringProducer(*ne.args[argIdx].value)`
                  // answered for the supplied arm just below (#3196:
                  // `argIdx` is the §22.5 binding's bound argument index
                  // for the slot, not the slot's positional index — the
                  // caller may have bound a NAMED argument).
                  // fix #4965 (§4.6 / §28.9 / §14.1, v0.307): a positional
                  // `(v: T)` field of the instantiation being constructed at
                  // `T = string` (the new type's substitution is the active
                  // one here) takes the same +1. `argExpr` is the CALL-SITE
                  // expression, typed in the caller's terms, so the producer
                  // test keeps its meaning: a `string`-typed producer argument
                  // is transferred, a `U`-typed one from a generic caller is
                  // never a recognised producer and is retained.
                  if (v->getType()->isPointerTy() &&
                      (isStringStorageMember(fs) ||
                       isTypeParamStringStorageMember(fs)) &&
                      !isPlusOneStringProducer(argExpr))
                      emitStringRetain(v);
                  builder_.CreateStore(
                      v, builder_.CreateStructGEP(
                             st, obj, idxIt->second));
              }
          }
      }
}

// fix #3742 (§22.8 step 1 / §22.10, v0.286) — see the declaration comment in
// LlvmBackend_impl.hpp for the measured double evaluation this exists to stop.
// Every conjunct here is one `emitImplicitBase` already applied before it
// lowered the arguments; stating them once is what keeps the evaluator and the
// skipper from disagreeing about which of them runs (§2.8.3 rule 4).
Emitter::BaseClauseExplicitCtor Emitter::baseClauseExplicitCtor(
        const sema::TypeSymbol* base,
        const std::vector<ast::CallArgument>& args) {
    BaseClauseExplicitCtor r;
    if (!base || args.empty()) return r;
    bool clauseAllPositional = true;
    for (const auto& a : args)
        if (!a.name.empty()) { clauseAllPositional = false; break; }
    r.cs = ctorForArgs(base, args);
    // fix #4520 (§22.5 / §22.8 step 1 / §22.10, v0.306): a NAMED clause is
    // resolved by SEMA, not by `ctorForArgs`.
    //
    // `ctorForArgs`'s `ctorTypeMatches` compares `args[i]`'s type class with
    // parameter `i`'s — a POSITIONAL pairing, and therefore a second §22.5
    // binding rule sitting beside the real one. It rejects every clause whose
    // names are correct but whose order is not: measured on 6c05b4cf,
    // `class D(v: int) : B(b := "e", a := v)` over
    // `constructor(a: int, b: string)` compared the string `"e"` against
    // `a: int`, returned null, and `emitImplicitBase` fell through to its
    // arity-0 fallback — so the base constructor was NEVER CALLED and the
    // clause's values were dropped entirely (the program printed `0` and a
    // blank line where `4` / `e` is correct). Re-keying the argument loop
    // alone would not have moved that program one byte.
    //
    // Sema published the §22.5 binding for this clause against this
    // constructor's own formal list, through the ONE binder body every
    // construction site runs; when `ctorForArgs` finds nothing for a NAMED
    // clause, that answer is adopted. Deliberately only when it finds
    // NOTHING, and only for a clause carrying a name: an all-positional
    // clause keeps `ctorForArgs` as its sole authority (including its #3760
    // type-class rejection, which is what stops a chain from binding a
    // wrongly-typed same-arity sibling), so no program that resolves today
    // can resolve differently here.
    if (!r.cs && !clauseAllPositional) {
        if (const sema::SemaContext::InheritanceClauseCtorBinding* pub =
                ctx_.inheritanceClauseCtorBinding(&args))
            r.cs = pub->ctor;
    }
    if (!r.cs) return r;
    auto it = ctorFns_.find(r.cs);
    // Declaring an imported constructor on demand is idempotent and is what
    // `emitImplicitBase` did at this point already; doing it here means both
    // readers see the same answer on the FIRST query, not only after the
    // emitter happened to have declared it for some other reason.
    if (it == ctorFns_.end() && ctx_.isImported(base)) {
        declareImportedCtor(r.cs);
        it = ctorFns_.find(r.cs);
    }
    // fix #3777 (§22.8 step 1 / §22.10 / §22.5, v0.286): the callable test is
    // against the resolved constructor's FORMAL count, not the base clause's
    // ARGUMENT count. `ctorForArgs` admits an omitted §22.5-defaulted tail (fix
    // #3082), so `class D(v: int) : P(v)` over `P.constructor(n: int, s: string
    // := "d")` resolved `r.cs` and then failed this guard — "resolved, but not
    // callable" — and `emitImplicitBase` fell through to its arity-0 fallback,
    // which found no arity-0 constructor either, so the base was NEVER
    // CONSTRUCTED: `s` stayed a null string pointer and the first read SEGV'd
    // (issue 3777, the §22.10 base-clause face of arm (b)). `emitImplicitBase`
    // now fills the omitted tail from the parameters' §22.5 defaults, exactly
    // as `emitChainCall` and every `new` site do.
    //
    // The test lives HERE rather than at the caller so that both readers of
    // this predicate — `emitImplicitBase`, which evaluates the clause
    // arguments, and `prePopulateBasePositionalFields`, which declines to
    // evaluate them a second time — keep answering identically (the #3742
    // contract).
    //
    // fix #4550 (§22.5 / §22.8 step 1 / §22.10, v0.306): a NAMED SHORT clause
    // is callable when sema published a §22.5 binding for THIS constructor.
    //
    // #3777 restricted the omitted-tail admission to an ALL-POSITIONAL clause,
    // and its reason was true at the time: "The caller's fill loop pairs clause
    // argument `i` with formal `i` POSITIONALLY, so it can only complete a
    // TRAILING omitted tail; declaring a named short clause callable would make
    // the evaluator skip it and the caller emit nothing." #4520 re-keyed that
    // loop by the §22.5 binding, and this commit gives it a PER-SLOT default
    // fill (`fillOmittedCtorSlots`), so both halves of the premise are gone —
    // but only for a clause sema actually bound. Without a binding the caller
    // still pairs by position, so the restriction stands for that case and a
    // named short clause simply stays "resolved, but NOT callable".
    //
    // MEASURED ON fcb4dbd5: `class D(v: int) : B(a := v)` over
    // `constructor(a: int, b: string := "zz")` compiled at exit 0 with no
    // diagnostic and printed `0` and a blank line where `4` / `zz` is correct
    // — `r.cs` resolved, this test failed it, and `emitImplicitBase` fell
    // through to its arity-0 fallback, which found nothing, so the base
    // subobject was NEVER CONSTRUCTED. The all-positional twin of the same
    // program prints `4` / `zz` (that is #3777's own fix), so the plumbing
    // works and it was the exact-arity rule that dropped it.
    //
    // The binding is checked for CONSTRUCTOR IDENTITY and for covering every
    // formal, the same discipline `emitImplicitBase` applies before reading it
    // — a disagreement costs the fix, never a wrong-slot store.
    const std::size_t nFormals = sema::formalParamCount(*r.cs);
    if (it == ctorFns_.end() || it->second->arg_size() != nFormals + 1)
        return r;  // resolved, but NOT callable: the caller still owes the args
    if (!clauseAllPositional && nFormals != args.size()) {
        const sema::SemaContext::InheritanceClauseCtorBinding* pub =
            ctx_.inheritanceClauseCtorBinding(&args);
        if (!pub || pub->ctor != r.cs || pub->paramToArg.size() != nFormals)
            return r;  // resolved, but NOT callable
    }
    r.fn = it->second;
    return r;
}

void Emitter::prePopulateBasePositionalFields(
        const sema::TypeSymbol* t,
        llvm::Value* obj,
        const std::vector<llvm::Value*>& ctorArgVals,
        const sema::ConstructorSymbol* cs,
        bool ctorArgsByRefAbi) {
    if (!t || !obj) return;

    // Find the first base class that has positional (primary-ctor) param fields.
    const sema::TypeSymbol* baseTs = baseClassOf(t);
    if (!baseTs) return;  // no base class, nothing to do

    // Check if the base class has any positional-param fields to populate.
    //
    // fix #3141 (family F3 of umbrella #3152, §11.1 / §22.8 / §2.8.3 rule 4):
    // this gate used to ask the base's `ast::TypeDecl`, which an IMPORTED base
    // never has — so the whole pre-population was skipped and the base
    // subobject's primary-ctor parameters stayed calloc-zero. Measured, with
    // `class PBox(V: int, W: int)` and `class D(): PBox(5, 6)`: `v=5 w=6` when
    // PBox was local, `v=0 w=0` when it was imported, compiling clean both
    // times. The list right below was ALREADY symbol-side; only the gate above
    // it died at the boundary. `hasPositionalFields` is mirrored onto the
    // TypeSymbol (#501) and a positional field is a FieldSymbol on both sides
    // (#2516), so both now read the symbol table.
    if (!baseTs->hasPositionalFields) return;

    // Collect the base's positional FieldSymbols in declaration order.
    std::vector<sema::FieldSymbol*> basePosFields;
    for (const sema::Symbol* m : sema::recordPositionalFieldSymbols(baseTs))
        basePosFields.push_back(
            const_cast<sema::FieldSymbol*>(
                static_cast<const sema::FieldSymbol*>(m)));
    if (basePosFields.empty()) return;

    // Determine the arg values to store into the base's positional fields.
    // We look for them from two sources:
    //   (a) TypeDecl::primaryBaseCtorArgs on the CURRENT class (parsed from
    //       the `: Base(args...)` inheritance clause).
    //   (b) The leading base(...) chain call in the explicit ctor body.
    // In both cases, evaluate the arg expressions in the current Emitter context
    // (where primary-param slots for `t` are already populated by the caller).

    std::vector<llvm::Value*> baseArgVals;
    // fix-forward #4981 (§4.6 / §28.9, v0.307): the source expression each
    // `baseArgVals` entry was lowered from, index-aligned with it, so the
    // store loop can tell a fresh +1 string (an interpolation, a call) from a
    // borrowed one before it adds the #4981 retain. Cleared with it.
    std::vector<const ast::Expr*> baseArgExprs;

    // fix #3562 (§22.8 step 0 / §22.5 / §22.10, v0.306): set when the explicit
    // `base(...)` arm below declines to fill `baseArgVals` because the call
    // binds a §22.10 convenience constructor whose chained-to primary runs
    // §22.8 step 0 for these slots. It suppresses the §22.5 DEFAULT loop as
    // well as the argument stores, because step 0 owns a slot's default
    // exactly as it owns its argument: §22.5 says the value an omitted
    // defaulted parameter is passed IS its default, so the default is part of
    // the one store step 0 makes, not a separate obligation this function
    // still owes. Without it, suppressing the argument fill leaves
    // `numToStore == 0` and the unconditional #3777 loop below reaches slots
    // it had never touched before — evaluating a non-constant default a
    // SECOND time and keeping the second value.
    bool chainedPrimaryOwnsSlots = false;

    // fix #4484 / fix #4491 (§22.5 "Optional parameters and named arguments"
    // / §22.8 step 0 / §22.10, v0.306): the §22.5 BINDING of the base call's
    // arguments to the base's positional slots, as sema published it. BOTH
    // arms of this function now have a channel, and they are different ones
    // because the two shapes have different identities:
    //
    //   case (b), the `base(...)` chain — `ExprTC::publishBaseChainPrimary
    //     Binding` → `SemaContext::setPositionalBinding`, keyed on the chain
    //     `CallExpr` (#4484, on the table #3583 widened to
    //     `const ast::Expr*`).
    //   case (a), the §22.10 inheritance clause — `ExprTC::visitTopDecl`'s
    //     primary-base-ctor-args walk → `SemaContext::
    //     setInheritanceClauseBinding`, keyed on the declaring
    //     `ast::TypeDecl` (#4491), because the clause's arguments are a bare
    //     `std::vector<ast::CallArgument>` with no `ast::Expr` node of their
    //     own to key on.
    //
    // Both are computed by the ONE binder body every other construction site
    // runs (`bindArgsToParams` over
    // `FormalsView::of(recordPositionalFieldSymbols(base))`) — the same one
    // the `new` site reads through `storePrimaryCtorPositionalArgs`.
    // Entry `pi` holds the index into `baseArgVals` — which is the base call's
    // SOURCE argument index, because the fill loops below push one value per
    // argument in source order — that supplies base positional field `pi`, or
    // -1 when the call omits that slot.
    //
    // EMPTY means "no binding published": every loop below then falls back to
    // the textual-order pairing `argument i ↔ field i` this function has
    // always used, which is bit-identical for an all-positional call and is
    // the pre-fix behaviour for every shape sema declines to bind. Since fix
    // #4496 the CHAIN arm's binder runs with `emit=true`, and since fix #4518
    // the CLAUSE arm's does too, so an over- or under-supplied, doubly-bound
    // or misnamed `base(...)` OR §22.10 inheritance clause is a §22.5 compile
    // error and never reaches this fallback. What still reaches it is the
    // shapes sema DECLINES TO BIND AT ALL rather than rejects: an imported
    // clause (no entry under the replayed vector's address), a call binding an
    // explicit base constructor (neither publisher runs there), and an
    // all-positional call, for which the fallback is the same answer.
    std::vector<int> baseFieldToArg;
    // The base positional field a given SOURCE argument index binds to, and
    // its inverse. Stated once because three places below need them: the §4.7
    // unit rescue (which must know the slot's TYPE), the argument store loop,
    // and the §22.5 default-fill loop (which must know which slots the call
    // did NOT supply — with named arguments that is no longer a trailing run).
    auto fieldForArg = [&](std::size_t ai) -> std::size_t {
        if (baseFieldToArg.empty()) return ai;
        for (std::size_t pi = 0; pi < baseFieldToArg.size(); ++pi)
            if (baseFieldToArg[pi] == static_cast<int>(ai)) return pi;
        return basePosFields.size();
    };
    auto argForField = [&](std::size_t pi) -> int {
        if (baseFieldToArg.empty())
            return pi < baseArgVals.size() ? static_cast<int>(pi) : -1;
        if (pi >= baseFieldToArg.size()) return -1;
        const int ai = baseFieldToArg[pi];
        return (ai >= 0 && static_cast<std::size_t>(ai) < baseArgVals.size())
                   ? ai
                   : -1;
    };

    // §22.8 step 1 / §22.10 / §11.1 (#3911/#3916): read the inheritance-
    // clause argument EXPRESSIONS via `primaryBaseCtorArgsOf`, NOT a raw
    // `dynamic_cast<ast::TypeDecl*>(t->decl)` — the latter is null for
    // every imported class (the #2374-class gap, deliberately load-bearing
    // elsewhere — see CLAUDE.md §6 / Symbol.hpp), and that nullity is
    // exactly the gap that dropped a generic imported MIDDLE ancestor's
    // OWN base-clause arguments (measured: `bp=0` instead of `bp=7` on
    // `class Derived(a): Mid<int>(a)` over an imported `Mid<T>(mp): Base(mp)`
    // because this case-a loop saw no `primaryBaseCtorArgs` to evaluate for
    // `Mid`, `baseArgVals` stayed empty, and the store loop below landed
    // nothing in `Base`'s `bp` slot).
    const std::vector<ast::CallArgument>* baseArgs =
        sema::primaryBaseCtorArgsOf(t);

    // --- Case (a): primary-ctor class with primaryBaseCtorArgs ---
    // fix #3742 (§22.8 step 1 / §22.10, v0.286): ...but NOT when the base call
    // resolves to an explicit base constructor that `emitImplicitBase` is about
    // to lower these very expressions for and CALL. That call's own §22.8
    // step 0 writes the same positional slots this loop would write, so
    // evaluating them here as well is a second evaluation of one source
    // expression — measured `bp=200 n=2` for `: Base(Counter.Next())` where
    // §22.8 step 1 admits `bp=100 n=1`, and a second heap allocation (a leak)
    // for `: Base(new Res())`. The predicate is `emitImplicitBase`'s own,
    // stated once (`baseClauseExplicitCtor`) and read from both sides.
    // `.fn`, not `.cs`: a resolved-but-not-callable constructor makes
    // `emitImplicitBase` fall through WITHOUT lowering anything, so the
    // pre-population still owes the arguments there.
    //
    // fix #4485 (§22.5 "Evaluation timing" / §22.8 step 0 / §22.10, v0.306):
    // computed ONCE, and its `.fn` truth also sets `chainedPrimaryOwnsSlots` —
    // this arm suppressed the ARGUMENT stores on the #3742 predicate but never
    // told the #3777 default-fill loop below, which is unconditional. A base
    // clause `: B(v)` binding a §22.10 convenience constructor of `B` (arity
    // matching the clause, chaining via `this(...)` into `B`'s primary) left
    // `baseArgVals` empty exactly as intended, but the fill loop then read
    // "every slot unsupplied" and materialized EVERY one of `B`'s primary
    // positional defaults here — a second evaluation of the one §22.8 step 0
    // already ran inside the call `emitImplicitBase` makes to that convenience
    // constructor. Same defect, same predicate, same repair as fix #3562 made
    // for case (b)'s `base(...)`-chain arm on these very lines — this is that
    // arm's case-(a) twin the #3562 commit deliberately left unfixed (scope
    // note). Measured on a tree with fix #3562 but not this one, `class
    // B(n: int, k: int := Counter.Next())` with `constructor(a: int)
    // this(a);` reached from `class D(v: int) : B(v)`: `k=2 cnt=2` where
    // §22.8 step 1 admits one evaluation (`k=1 cnt=1`); the `owned`-factory
    // form of the same default left the first instance's destructor never
    // run — LeakSanitizer reported the leaked allocation.
    const bool baseClauseHasArgs =
        t->kind == ast::TypeDeclKind::Class && baseArgs && !baseArgs->empty();
    const BaseClauseExplicitCtor baseClauseBce =
        baseClauseHasArgs ? baseClauseExplicitCtor(baseTs, *baseArgs)
                           : BaseClauseExplicitCtor{};
    if (baseClauseHasArgs && baseClauseBce.fn) chainedPrimaryOwnsSlots = true;
    if (baseClauseHasArgs && !baseClauseBce.fn) {
        // fix #4491 (§22.5 / §22.8 step 0 / §22.10, v0.306): install the
        // §22.5 binding sema published for THIS inheritance clause, before
        // anything below reads `fieldForArg` / `argForField`. This is case
        // (a) — the `: B(s := "c", n := v)` spelling — the twin of the
        // `base(...)` arm #4484 closed, and it mispaired identically:
        // measured on e9239ab4, `class D(v: int) : B(s := "clause", n := v)`
        // over `class B(n: int, s: string)` printed `5811392` for `n` (the
        // string pointer's low 32 bits) and then SEGVed at `0x11` inside
        // `bz_string_to_utf8z`.
        //
        // The channel is `SemaContext::inheritanceClauseBinding`, keyed on
        // the ADDRESS of the clause's argument vector, because the clause has
        // no `ast::Expr` node to key on (see SemaContext.hpp for why a third
        // key on the `const ast::Expr*` table would collide with a clause
        // argument that is itself a `new` expression).
        //
        // `baseArgs` is exactly that key: it is what
        // `sema::primaryBaseCtorArgsOf(t)` returned a few lines above, which
        // is `&td->primaryBaseCtorArgs` for a LOCAL class and the
        // ModuleLoader-replayed `&t->importedPrimaryBaseCtorArgs` (#3911) for
        // an imported one. Sema publishes only for the local vector, so the
        // local/imported split here is POINTER IDENTITY — an imported class
        // finds no entry and keeps the textual-order fallback — with no
        // `dynamic_cast<const ast::TypeDecl*>(t->decl)` to add a #3152 seam
        // read, and no second guard to keep in sync with the accessor.
        if (const std::vector<int>* pub =
                ctx_.inheritanceClauseBinding(baseArgs))
            baseFieldToArg = *pub;
        // §22.5 "Evaluation order": the supplied arguments run in call-site
        // TEXTUAL order "for positional and named arguments alike", so this
        // loop stays source-indexed and `baseArgVals` stays source-indexed;
        // only the STORE below is re-keyed by the binding (fix #4491, the
        // same split #4484 made on the `base(...)` arm).
        for (std::size_t ai = 0; ai < baseArgs->size(); ++ai) {
            const auto& arg = (*baseArgs)[ai];
            if (!arg.value) { baseArgVals.clear(); baseArgExprs.clear(); break; }
            llvm::Value* v = lowerExpr(*arg.value);
            // fix #3502 (§4.7 / §22.10 / §11.1, v0.283): the inheritance-clause
            // face of the same gap — see the `storePrimaryCtorPositionalArgs`
            // site above. A null `v` here used to ABANDON the whole
            // pre-population (`clear(); break;`), so `class D(): B(C.Nothing())`
            // left EVERY one of B's positional fields at its calloc zero.
            // fix #4491: the slot whose TYPE the §4.7 rescue must read is the
            // one §22.5 BINDS this argument to, not the one at its source
            // position — `: B(u := C.Nothing(), n := 1)` must rescue against
            // the `u` slot's unit type. `fieldForArg` degenerates to the
            // source index when no binding was published, which is the
            // pre-#4491 `baseArgVals.size()` this replaced (every iteration
            // either pushes or breaks, so the two agree exactly).
            if (!v) {
                const std::size_t fi = fieldForArg(ai);
                if (fi < basePosFields.size())
                    v = unitValueForTarget(
                        v, recordMemberTypeResolved(basePosFields[fi]),
                        arg.value.get());
            }
            if (!v) { baseArgVals.clear(); baseArgExprs.clear(); break; }
            baseArgVals.push_back(v);
            baseArgExprs.push_back(arg.value.get());
        }
    }
    // --- Case (b): explicit ctor with leading base(...) chain call ---
    // fix #3523 (§22.10 / §22.8 step 0 / §12.8, v0.283): the guard used to
    // require `!cs->params.empty()`, so a ZERO-parameter derived constructor
    // never reached the scan below and its base's positional slots kept their
    // calloc zero — `class D: B` with `constructor() base(7);` over
    // `class B(n: int)` read 0, while the otherwise identical `class E: B`
    // with `constructor(v: int) base(v);` read the right value. Nothing in
    // §22.8 step 0 or §12.8 makes the base's slots depend on the DERIVED
    // constructor's own arity. The clause's only purpose is to bind that
    // constructor's parameters into `paramSlots_` so a chain argument that
    // READS one can be lowered; a chain call whose arguments read nothing
    // needs no bindings, which is exactly the zero-parameter case, and the
    // binding loop below is `pi < cs->params.size()` so it runs zero times
    // there. `emitChainCall` does not cover for it either: for a base with no
    // explicit constructor it takes the `!isThis && !hasAnyConstructor`
    // branch, which runs the base's field initializers and never lowers the
    // chain arguments.
    else if (cs && (cs->params.empty() ||
                    (!ctorArgVals.empty() &&
                     cs->params.size() == ctorArgVals.size()))) {
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (cd && !cd->body.empty()) {
            bool isThis = false;
            const ast::CallExpr* chainCall = chainCallOf(*cd->body[0], isThis);
            // fix #3562 (§22.8 step 0 / §22.10 / §12.8, v0.306): ...but NOT
            // when the `base(...)` call binds to an explicit constructor of
            // the base that chains via `this(...)`. This is case (a)'s #3742
            // question — "is `emitChainCall` about to lower these very
            // expressions and CALL a constructor whose own §22.8 step 0 writes
            // the same positional slots?" — asked on the arm that never asked
            // it. The fill loop below pairs chain argument `i` with the base's
            // positional field `i`, which describes the base's PRIMARY
            // parameter list; a §22.10 convenience constructor's parameter
            // list is a DIFFERENT list, so position `i` does not name field
            // `i` at all. §22.8 puts step 0 for such a call inside the
            // chained-to primary ("a constructor that chains via `this(...)`
            // skips steps 0 and 2 … primary-parameter capture happens inside
            // the chained-to primary"), which `emitChainCall` reaches through
            // the ctor call, so this pre-population is redundant there as well
            // as wrong.
            //
            // Measured on 7f29b8ce: `class B(x: int, y: int)` +
            // `constructor(a: int, d: decimal) this(a, 42);` reached from
            // `class D : B` with `constructor() base(1, 7.5m);` emitted
            // `store %bz.decimal …, ptr <GEP y>` — a 16-byte store into the
            // 4-byte `y` slot at offset 16 of a 24-byte object, i.e. an
            // 8-byte heap-buffer-overflow that ASan reports as
            // "WRITE of size 16 … 7 bytes after 24-byte region". The narrower
            // ptr-into-int form of the same store (`s: string` in that
            // position) corrupts the trailing padding silently — ASan's
            // 8-byte fast path reads the granule the misaligned store starts
            // in and misses it — which is why the oracle for this is the
            // 16-byte one.
            //
            // The predicate is §22.8's own, the one #3583 put at the `new`
            // site, over the SAME constructor `emitChainCall` will select
            // (`ctorForArgs(targetCls, call->args)` there for a positional
            // chain). It answers false for a null target (the call goes
            // straight to the primary — the #3523 shape, where this
            // pre-population is still owed) and false for an imported base
            // constructor (SEAM-DEBT #3625: no symbol-side channel carries a
            // constructor body's chain form), so both keep today's behaviour.
            //
            // WHY `.fn` IS NOT ASKED HERE, unlike case (a)'s #3742 guard.
            // That guard declines only for a resolved AND CALLABLE base
            // constructor, because a resolved-but-not-callable one makes
            // `emitImplicitBase` fall through without lowering anything and
            // the pre-population still owes the arguments. This predicate has
            // no such hole to cover: `ctorForArgs` is the SAME selector
            // `emitChainCall` uses for a positional chain, and `emitChainCall`
            // — unlike `emitImplicitBase` — has no callability precondition to
            // fail; on a `ctorFns_` miss or a false `fillOmittedCtorArgs` it
            // abandons the call, which would leave these slots calloc-zero.
            // No program reaching that state was constructible: the target
            // here is a LOCAL constructor (`ctorChainsViaThis` needs an
            // `ast::ConstructorDecl` body), and a local constructor's function
            // is declared for every class codegen emits. Recorded rather than
            // guarded, because adding `.fn`'s callability test would re-admit
            // the mispairing for exactly the shapes it is meant to exclude.
            const bool baseCallChainsThis =
                chainCall && !isThis && !chainCall->args.empty() &&
                ctorChainsViaThis(ctorForArgs(baseTs, chainCall->args,
                                              nullptr, /*chainCall=*/true));
            if (baseCallChainsThis) chainedPrimaryOwnsSlots = true;
            if (chainCall && !isThis && !chainCall->args.empty() &&
                !baseCallChainsThis) {
                // fix #4484 (§22.5 / §22.8 step 0 / §22.10, v0.306): install
                // the §22.5 binding sema published for THIS `base(...)` call
                // before anything below reads `fieldForArg` / `argForField`.
                // This is the arm the issue names: the call binds the base's
                // PRIMARY parameter list (the `ctorChainsViaThis` arm above
                // has already taken every call that binds an explicit base
                // constructor out of this path), and those parameters are
                // exactly `basePosFields` — so a named argument names one of
                // them and §22.5 binds it there, wherever it sits in the
                // argument list.
                if (const std::vector<int>* pub =
                        ctx_.positionalBinding(*chainCall))
                    baseFieldToArg = *pub;
                // Temporarily bind ctor params to their values so that the
                // chain-call arg expressions (which reference ctor params) can
                // be evaluated correctly. Save and restore paramSlots_.
                // fix #2584 / §22.3 (v0.236): when ctorArgsByRefAbi (the
                // top-level `new`-expression path), ctorArgVals[i] is the
                // caller's storage ADDRESS for a ref/out param (CG50), not a
                // value — binding that address in paramSlots_ and reading the
                // param loads the POINTER itself, so a chain-call arg that
                // reads the ref param (`base(n + 1)`) emits `add ptr, i32`
                // (verifyModule rejects). Register the address in refParams_
                // instead so reads dereference through it, mirroring
                // bindParameters and #2283's inline-ctor path. The recursion
                // (ctorArgsByRefAbi=false) passes lowered VALUES, so it keeps
                // the paramSlots_ binding it always used.
                auto savedParams = paramSlots_;
                auto savedRefParams = refParams_;  // fix #2584
                for (std::size_t pi = 0; pi < cs->params.size(); ++pi) {
                    auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                        cs->params[pi]);
                    if (ctorArgsByRefAbi && ps && isByRefMod(ps->modifier)) {
                        llvm::Type* elemTy =
                            ps->declType ? lowerType(ps->declType) : i32();
                        refParams_[cs->params[pi]] = {ctorArgVals[pi], elemTy};
                    } else {
                        auto* slot = entryAlloca(ctorArgVals[pi]->getType());
                        builder_.CreateStore(ctorArgVals[pi], slot);
                        paramSlots_[cs->params[pi]] = slot;
                    }
                }
                // §22.5 "Evaluation order": the SUPPLIED arguments are
                // evaluated in call-site TEXTUAL order, "for positional and
                // named arguments alike; naming an argument changes which
                // parameter it binds to, never when it runs". So this loop
                // stays in source order and `baseArgVals` stays source-
                // indexed; only the STORE below is re-keyed by the binding.
                for (std::size_t ai = 0; ai < chainCall->args.size(); ++ai) {
                    const auto& arg = chainCall->args[ai];
                    if (!arg.value) { baseArgVals.clear(); baseArgExprs.clear(); break; }
                    llvm::Value* v = lowerExpr(*arg.value);
                    // fix #3502 (§4.7 / §22.10 / §11.1, v0.283): the explicit
                    // `base(...)`-chain face of the same gap — same reasoning
                    // and same pairing as the primaryBaseCtorArgs loop above.
                    // fix #4484: the slot whose TYPE the §4.7 rescue must
                    // read is the one §22.5 BINDS this argument to, not the
                    // one at its source position — `base(u := C.Nothing(),
                    // n := 1)` must rescue against the `u` slot's unit type.
                    if (!v) {
                        const std::size_t fi = fieldForArg(ai);
                        if (fi < basePosFields.size())
                            v = unitValueForTarget(
                                v, recordMemberTypeResolved(basePosFields[fi]),
                                arg.value.get());
                    }
                    if (!v) {
                        baseArgVals.clear();
                        baseArgExprs.clear();
                        break;
                    }
                    baseArgVals.push_back(v);
                    baseArgExprs.push_back(arg.value.get());
                }
                paramSlots_ = std::move(savedParams);
                refParams_ = std::move(savedRefParams);  // fix #2584
            }
        }
    }

    // Store the base-call arg values into the base's positional-param field slots.
    // fix #4484 (§22.5 / §22.8 step 0 / §22.10, v0.306): the pairing is
    // `argForField` — the §22.5 binding when sema published one for this base
    // call, the textual-order `argument i ↔ field i` otherwise. This loop used
    // to run `for i in [0, min(baseArgVals.size(), basePosFields.size()))` and
    // store `baseArgVals[i]` into `basePosFields[i]`, which is the same thing
    // for an all-positional call and a wrong-slot store for a named one.
    //
    // fix #4981 (§14.1 / §22.5 / §22.10 / §22.8 step 0, v0.307): install the
    // base's generic-parameter substitution (`baseTs->genericParams[i]` →
    // `lowerTypeRef(t->baseTypeRefs[bi]->genericArgs[i])`) before `classStruct`,
    // so the returned layout is the SPECIALIZED `class.B$specN` (T→ptr for
    // `B<string>`) instead of the GENERIC `class.B` (T→i32 placeholder).
    // Without this, the GEP+store below writes 8 bytes at slot 2 of a struct
    // whose slot 2 is `i32` — for `class E: B<string> { ctor(s) base(s); }` with
    // `class B<T>(v: T)`, the 16-byte `%class.B` has slot 2 at offset 12 typed
    // `i32`, but the actual allocation is 24-byte `%class.E$spec0` with slot 2
    // at offset 16 typed `ptr`. The 8-byte ptr store writes into padding and
    // the real slot stays calloc-zero; the BASE then SEGVs in
    // `bz_string_to_utf8z` reading the zero slot, and #4965's disposer release
    // for `isTypeParamStringStorageMember` SEGVs in `bz_string_release` when
    // it tries to release a corrupted pointer. The subst must also be saved
    // and restored so callers above this site (e.g. the outer `lowerNew` that
    // lowered ctor args under the caller's subst) are unaffected.
    auto savedBaseStSubst = genericSubst_;
    auto savedBaseStSemaTy = genericSubstSemaTy_;
    if (!baseTs->genericParams.empty()) {
        // Find the base ref that pins `baseTs` to its concrete type args.
        const sema::TypeRef* baseRef = nullptr;
        for (std::size_t bi = 0; bi < t->baseTypes.size(); ++bi) {
            if (t->baseTypes[bi] == baseTs) {
                baseRef = (bi < t->baseTypeRefs.size())
                              ? t->baseTypeRefs[bi] : nullptr;
                break;
            }
        }
        if (baseRef) {
            // fix #3143: shared walk — same one the §14.1 classStruct path uses.
            const std::vector<std::string_view> gpNames =
                sema::genericParamNames(baseTs);
            if (!gpNames.empty() &&
                gpNames.size() == baseRef->genericArgs.size()) {
                for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                    const sema::TypeRef* arg = baseRef->genericArgs[gi];
                    if (!arg) continue;
                    genericSubst_[gpNames[gi]] = lowerTypeRef(arg);
                    genericSubstSemaTy_[gpNames[gi]] = arg;
                }
            }
        }
    }
    llvm::StructType* baseSt = classStruct(baseTs);
    // The subst STAYS installed through the store loops and the recursion
    // below: `isTypeParamStringStorageMember(fs)` resolves the bare-`T` field
    // type through the active map (otherwise it stays blind at the
    // `T → i32` placeholder and never fires the +1 retain); and the
    // default-fill loop below forwards `genericSubst_` to
    // `positionalParamDefaultValue` for the same reason. The recursion
    // inherits it correctly too — its own `baseClassOf(baseTs)` is its own
    // `t`, so a level deeper sees a fresh subst layered over this one.
    // The restore is at the very end of the function.
    for (std::size_t i = 0; i < basePosFields.size(); ++i) {
        const int ai = argForField(i);
        if (ai < 0) continue;
        sema::FieldSymbol* fs = basePosFields[i];
        auto idxIt = fieldIndex_.find(fs);
        if (idxIt == fieldIndex_.end()) continue;
        llvm::Value* v = baseArgVals[static_cast<std::size_t>(ai)];
        if (!v) continue;
        // fix #4981 (§4.6 / §28.9 / §14.1, v0.307): the producer's #4965 fix
        // retains in `storePrimaryCtorPositionalArgs`, which fires for `t`'s
        // OWN positional fields. For a NON-GENERIC derived class over a
        // GENERIC base (`class E: B<string> { ctor(s) base(s); }`, with
        // `class B<T>(v: T)`), `t = E` has no positional fields, that helper
        // returns early, and the actual write to `B.v` happens HERE — without
        // a +1 of its own, the dispose-side release for
        // `isTypeParamStringStorageMember` then over-releases the
        // caller-owned string (the producer's intermediate crashed with the
        // same heap-buffer-overflow class but in `bz_string_release`; once
        // the layout fix above lands, the symptom becomes "refcount
        // over-release"). The slot is `B`'s positional field `(v: T)`
        // resolved through the active subst: when `T = string` the receiver
        // owns the value, and the `isTypeParamStringStorageMember` predicate
        // is the matching release side (#4965 / Setup.cpp / Disposal.cpp).
        // Symmetric to that fix's ctor-body retain. The producer-check
        // (`isPlusOneStringProducer` on the source expr) is conservatively
        // omitted — the call-shape that reaches this site is a base call
        // from a `ctor(s: string) base(s)` over a primary-ctor positional
        // field, and a `+1` producer there is unusual; widening this gate is
        // filed separately rather than guessed at here.
        //
        // fix-forward #4981 (§4.6 / §28.9, v0.307): that omission was wrong —
        // the gate's plain leg caught it. A +1 producer (`$"v={a}"` in
        // `class DInterp(a: int): BaseS($"v={a}")`) already hands the slot
        // its one reference; retaining it again made it +2 against the single
        // dispose-side release and leaked the string on every construction
        // (row base_clause_new_argument_3742.control_value_shaped_base_clause_
        // arguments_unchanged, LSan: 32 B direct + 4 B indirect). So the
        // retain fires only for a BORROWED source — the same
        // `isPlusOneStringProducer` gate the other string-slot stores use
        // (e.g. LlvmBackend_Assign.cpp's field-assign path). An entry with no
        // recorded expression keeps the unconditional retain.
        const ast::Expr* srcExpr =
            static_cast<std::size_t>(ai) < baseArgExprs.size()
                ? baseArgExprs[static_cast<std::size_t>(ai)] : nullptr;
        if (v->getType()->isPointerTy() &&
            (isStringStorageMember(fs) ||
             isTypeParamStringStorageMember(fs)) &&
            !(srcExpr && isPlusOneStringProducer(srcExpr)))
            emitStringRetain(v);
        llvm::Type* fieldTy = baseSt->getElementType(idxIt->second);
        v = coerceNumericToType(v, fieldTy, /*srcUnsigned=*/false);
        builder_.CreateStore(v, builder_.CreateStructGEP(baseSt, obj, idxIt->second));
    }

    // fix #3777 (§22.5 "Optional parameters and named arguments" / §22.10 /
    // §22.8 step 0, v0.286): every positional slot the base call did NOT supply
    // takes its §22.5 DEFAULT. This loop is what completes this function's own
    // contract — "the base's positional-param field slots hold the values the
    // base call passed" — because §22.5 says the value an omitted defaulted
    // parameter is passed IS its default. Without it the slot kept its calloc
    // zero and the base's field initializers, and every later read, saw it:
    //
    //   class P(n: int, s: string := "d") ... end P;
    //   class D : P  public constructor(v: int) base(v); end constructor; end D;
    //   new D(7).Show()      // SEGV — `s` is a null string pointer
    //   class D(v: int) : P(v)                      // the §22.10 base-clause
    //                                               // face: the same SEGV
    //
    // (issue 3777 arm (b) and its base-clause twin; measured on r12-cycle-1.)
    // The `int` twin was non-fatal and therefore worse — it read back 0 rather
    // than the declared default, compiling clean.
    //
    // WHICH SLOTS THIS LOOP OWES. Every slot the base call did not SUPPLY,
    // which `argForField` answers per slot.
    //
    // ⚠ fix #4484 (§22.5 / §22.8 step 0 / §22.10, v0.306) corrects what this
    // paragraph used to assert. It said "a base call with NAMED arguments
    // publishes no binding at this site, so it produces no `baseArgVals` at
    // all and reaches this loop with `numToStore == 0`". That was FALSE of
    // case (b): the explicit `base(...)` chain arm above lowers
    // `chainCall->args` unconditionally, named or not, so a
    // named base call arrived here with a FULL `baseArgVals` that the store
    // loop then paired by textual position. ⚠ fix #4491 removes the other
    // half of that sentence too: case (a) — the §22.10 inheritance clause —
    // lowers ITS arguments unconditionally as well, and now publishes a
    // binding of its own (`SemaContext::inheritanceClauseBinding`), so
    // "no `SemaContext` binding at all" no longer describes it either.
    // Measured on 4d9d186a, `base(s :=
    // "x", n := 1)` against `class B(n: int, s: string)` printed `4627856`
    // for `n` — the low 32 bits of the string pointer — and SIGSEGVed in
    // `bz_string_to_utf8z` dereferencing the integer `1` as a string; with a
    // `decimal` in the trailing slot the same mispairing is a clean
    // "heap-buffer-overflow … WRITE of size 16 … 11 bytes after 32-byte
    // region" (issue 4484). The comment described ONE arm as though it
    // described the function, which is how the defect stayed hidden through
    // #3562's work on the very same lines.
    //
    // Since #4484 the supplied slots are whatever §22.5 bound, so they are no
    // longer necessarily a prefix `[0, numToStore)` — `base(s := "x")`
    // against `class B(n: int := 1, s: string)` supplies slot 1 and owes slot
    // 0 its default. This loop therefore asks `argForField` per slot instead
    // of starting at a watermark. For a call sema declined to bind, or an
    // all-positional one, `argForField` answers exactly the old watermark.
    //
    // Runs BEFORE the `baseArgVals.empty()` early return on purpose: a `base()`
    // with no arguments at all into a base whose positional parameters are ALL
    // defaulted is exactly the same defect (measured: SEGV), and it produces no
    // `baseArgVals`.
    // fix #3562 (§22.5 / §22.8 step 0 / §22.10, v0.306): ...but NOT for a
    // base call whose chained-to primary runs step 0 for these slots — see
    // `chainedPrimaryOwnsSlots` above. Measured on the intermediate state that
    // suppressed only the argument stores, with `class B(n: int, k: int :=
    // Counter.Next())` reached through `constructor(w: int, v: int) this(v);`
    // from `base(9, 3)`: `k=2 cnt=2` where §22.8 step 1 admits one evaluation
    // and `k=1 cnt=1`. The `owned` form of the same default (§22.5 admits a
    // factory returning an owned instance) leaked the first object outright —
    // "LeakSanitizer: 16 byte(s) in 1 object(s)" — because the second store
    // overwrote it before any destructor could run.
    for (std::size_t i = 0;
         !chainedPrimaryOwnsSlots && i < basePosFields.size(); ++i) {
        if (argForField(i) >= 0) continue;  // supplied — fix #4484
        sema::FieldSymbol* fs = basePosFields[i];
        auto idxIt = fieldIndex_.find(fs);
        if (idxIt == fieldIndex_.end()) continue;
        // The ONE §22.5 positional-default materialization (local `:= Expr`,
        // imported rendered text) — shared with §22.8 step 0's own store,
        // `storePrimaryCtorPositionalArgs`. Null means "no default declared":
        // leave the slot calloc-zero, exactly as before this fix.
        llvm::Value* v = positionalParamDefaultValue(*fs, obj, genericSubst_,
                                                     genericSubstSemaTy_);
        if (!v) continue;
        llvm::Type* fieldTy = baseSt->getElementType(idxIt->second);
        v = coerceNumericToType(v, fieldTy, /*srcUnsigned=*/false);
        builder_.CreateStore(v, builder_.CreateStructGEP(baseSt, obj, idxIt->second));
    }

    if (baseArgVals.empty()) {
        // No base-call args determined — nothing more to pre-populate.
        // Still recurse in case the base's own base needs pre-population.
        prePopulateBasePositionalFields(baseTs, obj, {}, nullptr);
        return;
    }

    // Recurse: the base class itself may also have a base with positional params
    // (e.g. C(z): B(z): A(z) — B's and A's params both need pre-population).
    // Find what ctor symbol the base class has (if any) for use in recursion.
    //
    // fix #4484 note: `baseArgVals` is handed down in SOURCE order, which the
    // recursion's own case (b) would bind to `baseCs->params[pi]` by position.
    // That is NOT re-keyed by the §22.5 binding above, deliberately: the shape
    // is unreachable. `baseCs` is non-null only when the base declares an
    // explicit non-static constructor of this arity, and sema publishes a
    // binding for this call only when it declares NONE (see
    // `publishBaseChainPrimaryBinding`) — so whenever `baseFieldToArg` is
    // non-empty, `baseCs` is null and `ctorArgVals` goes unread one level
    // down. Widening the pairing here on an unreachable path would be an
    // untestable change. fix #4491 note: the clause arm's publication gate is
    // the SAME arity question (no explicit non-static base constructor of the
    // clause's arity, see `ExprTC::visitTopDecl`), so the argument holds
    // unchanged now that case (a) can also fill `baseFieldToArg`.
    const sema::ConstructorSymbol* baseCs = nullptr;
    if (!baseArgVals.empty()) {
        baseCs = ctorByArity(baseTs, baseArgVals.size(), nullptr);
    }
    prePopulateBasePositionalFields(baseTs, obj, baseArgVals, baseCs);
    // fix #4981: restore the outer subst before returning (the recursive call
    // installs its own).
    genericSubst_ = std::move(savedBaseStSubst);
    genericSubstSemaTy_ = std::move(savedBaseStSemaTy);
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


// fix #2722 / stdlib §4.8/§4.9/§4.10 (v0.240).
//
// WHY A DIAGNOSTIC AND NOT AN IMPLEMENTATION. The comparer overloads ARE
// specified — §4.10 declares two of them on `Set<T>` and on `OwningSet<T>`,
// §4.8/§4.9 the same pair on `Dictionary<K, V>`/`OwningDictionary<K, V>`, and
// §4.8's "Stability while stored" paragraph names "the collection's
// `IEqualityComparer` (hash forms)" as what governs lookup — so this is NOT the
// #2693 `OwningLinkedList<T>(source)` case of an unspecified form invented by
// codegen. It is nonetheless deferred rather than implemented here, for two
// reasons that are recorded so the decision can be revisited. Both are purely
// MECHANICAL — no design question is open, and an implementer should not go
// looking for a ruling (see the note after them):
//
//  1. The runtime table has nowhere to put a comparer. `bz_dict_alloc_generic`
//     (bezel_runtime_collections.cpp:719) takes bare
//     `uint64_t (*)(const void*)` / `bool (*)(const void*, const void*)`
//     function pointers with NO user-data parameter, and `bz_dict_t`
//     (bezel_runtime_collections.cpp:369) has no context field. Threading an
//     instance through means widening the struct and all eight `hash_fn`/`eq_fn`
//     call sites, plus the dict→dict clone at :1298 — a runtime change, not a
//     codegen one.
//  2. Dispatching to the comparer from a trampoline is not yet safe. The
//     existing builders (buildValueHashTrampoline / buildValueEqTrampoline,
//     LlvmBackend_Calls3.cpp:393/440) save only the insert point and
//     `currentFn_`; they do not reset `propagateBB_`/`unwindTarget_`/
//     `disposeStack_`. An interface call goes through `emitMethodCall`, which
//     calls `emitPendingCheck()` and would splice unwind edges belonging to the
//     ENCLOSING user function into the trampoline. The iterator/async lowerers
//     show the required reset idiom, but adopting it here is its own change.
//
// THE COMPARER'S LIFETIME IS ALREADY SETTLED — do not treat it as an open
// question. An earlier draft of this comment listed it as a third, blocking
// reason (that holding a borrowed comparer for the collection's whole life
// needed a ruling). That was wrong and is withdrawn: the stdlib's own reference
// implementation of `OwningDictionary<K, V>` declares `private _comparer:
// IEqualityComparer<K>;` — an unmarked, i.e. BORROWING, field held for exactly
// that lifetime — and calls `_comparer.Equals(...)` from inside `Add`
// (Bezel_Standard_Library.md:2040 and :2057/:2083), and spec §9.7 confirms a
// direct class-typed borrow field is a supported, statically checked construct.
// The borrow reading is therefore already modelled normatively; implement it
// rather than seeking a new ruling or proposing a stdlib amendment.
//
// Sema already records the same deferral in prose (SemaContext.hpp: "the
// Dictionary/Set comparer-constructor overloads that would *consume* one are a
// separate, still-deferred feature"). What was missing is that the deferral was
// SILENT: codegen accepted the call and produced a wrong answer. Per the #511
// `GetType` precedent, make it loud instead.
//
// SortedSet (§4.18) / SortedDictionary (§4.17) are deliberately NOT covered:
// §4.17 states they order by `K`'s `IComparable<K>.CompareTo` and that "there is
// no separate comparer parameter", so what a comparer argument means there is a
// different question, filed rather than answered here.
bool Emitter::rejectUnimplementedComparerCtor(const ast::NewExpr& ne,
                                              std::string_view colName) {
    // The section that DECLARES the overload, per type: §4.10 covers "Set<T> and
    // OwningSet<T>" together, but the dictionary pair is split — §4.8 is
    // `Dictionary<K, V>` and §4.9 is `OwningDictionary<K, V>`. Deriving it here
    // rather than taking it from the call site is what keeps the two dictionary
    // names from collapsing onto one section.
    const char* sec = nullptr;
    if (colName == "Set" || colName == "OwningSet")   sec = "§4.10";
    else if (colName == "Dictionary")                 sec = "§4.8";
    else if (colName == "OwningDictionary")           sec = "§4.9";
    else return false;
    if (ne.args.empty() || !ne.args[0].value) return false;
    // Which forms carry a comparer: `(capacity, comparer)` is the only
    // two-argument constructor either family declares, and a lone argument is
    // the comparer form exactly when it is not the `capacity: int` overload.
    //
    // POLARITY. The discriminator mirrors the negative-capacity guard's
    // non-nullable-int test, and — deliberately — mirrors its behavior on an
    // UNKNOWN argument type too: a null `exprType` counts as NOT the comparer
    // form, so it is skipped rather than rejected. Inverting that (treating
    // unknown as a comparer) would turn any argument sema could not type into a
    // hard error on a program that compiles today, and a false reject is worse
    // than the silent drop this guard exists to catch — the drop is merely the
    // pre-existing status quo for such an argument.
    //
    // WIDTH. The capacity form is every non-nullable integral primitive that
    // spec §5.1 (v0.240) IMPLICITLY converts to `int`, not `int` alone:
    //
    //     sbyte -> short, int, long, …          byte  -> short, ushort, int, …
    //     short -> int, long, …                 ushort -> int, uint, long, …
    //     char  -> int, uint, long, …
    //
    // so `var b: byte := 8; new Set<int>(b)` is a well-typed call against the
    // declared `constructor(capacity: int)`. Matching on `Int` alone classified
    // it as the comparer form and hard-rejected a program that compiles today —
    // a false reject carrying a WRONG explanation, which is the worse half.
    // Nothing upstream rescues it: the builtin collections have
    // `declaration == nullptr` so sema's `checkNewExprArgs` never runs, and
    // `exprType` reports the argument's NATURAL type, never a target-converted
    // one, so the widening is invisible here unless it is spelled out.
    //
    // `uint`, `long` and `ulong` are deliberately NOT accepted: §5.1's
    // never-implicit list covers signed/unsigned of the same or narrower width
    // and every narrowing, so none of them converts to `int` AS A RUNTIME VALUE.
    // They stay on the comparer side, where the pre-existing negative-capacity
    // guard also ignores them.
    //
    // ⚠ That is true of the IMPLEMENTATION, not of the spec. §5.1's *Constant
    // narrowing* clause (spec line 667) also lets a compile-time CONSTANT of
    // integer type convert to any integer type whose range contains its value,
    // so `const N: long := 8; new Set<int>(N)` is a well-typed capacity call by
    // the spec and is rejected here. It is not a live regression: sema
    // implements constant narrowing only when the source's static type is
    // exactly `Int`, and never *to* `Int`, so the compiler already rejects that
    // conversion everywhere — this predicate merely inherits the existing gap
    // rather than widening it. Filed separately; if sema ever gains the spec's
    // full constant-narrowing rule, this set must widen with it.
    //
    // NULLABILITY is deliberately NOT tested. §7.4/§12.7 flow narrowing (#914)
    // proves a nullable value non-null in value position, and
    // ExprTypecheck.cpp's implementation says verbatim that it treats the
    // narrowed source as non-nullable "WITHOUT rewriting its exprType" — so
    // `var n: int? := 8; if n is not null then new Set<int>(n)` still reports
    // `nullable == true` here, and a `!nullable` conjunct rejected a program
    // that compiles and runs correctly (arg 0 was simply never lowered on the
    // capacity path — capacity is only a hint). Dropping the conjunct cannot
    // re-open the silent drop this guard exists to catch: `IEqualityComparer<T>`
    // is an INTERFACE, hence `Kind::Named`, while every capacity primitive is
    // `Kind::Primitive` — the two sets are disjoint by construction, nullable or
    // not. The pre-existing negative-capacity guards keep their own `!nullable`
    // test, which is theirs to keep: they must LOWER the argument, and a
    // tagged-optional slot is not an i64.
    auto isCapacityPrimitive = [](sema::PrimitiveType p) {
        switch (p) {
            case sema::PrimitiveType::SByte:
            case sema::PrimitiveType::Byte:
            case sema::PrimitiveType::Short:
            case sema::PrimitiveType::UShort:
            case sema::PrimitiveType::Char:
            case sema::PrimitiveType::Int:
                return true;
            default:
                return false;
        }
    };
    bool comparerForm = ne.args.size() >= 2;
    if (!comparerForm) {
        const sema::TypeRef* a0ty = ctx_.exprType(*ne.args[0].value);
        comparerForm = a0ty && !(a0ty->kind == sema::TypeRef::Kind::Primitive &&
                                 isCapacityPrimitive(a0ty->primitive));
    }
    if (!comparerForm) return false;
    ctx_.diagnostics().report(
        Severity::Error, ne.span.begin,
        "stdlib " + std::string(sec) + ": the '" + std::string(colName) +
        "' IEqualityComparer constructor is not yet implemented — the comparer "
        "would be silently ignored and default equality used instead. Omit it "
        "and give the element type its own Equals/GetHashCode (spec §8.3).");
    return true;
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
    // §22.4 / §28.3 (#1833 (b), #1467 sibling): snapshot the
    // discardOwnedOutSlots_ stack at the entry of `lowerNew` so the
    // statement-context `out _` slot a ctor's `out owned` parameter
    // writes into (recorded by buildOrderedArgs, dispatched by lowerCall
    // for method calls) is disposed here exactly the same way — under
    // `!diverged_`, null-guarded, via dynamic vtable Dispose + reclaim.
    // Without this, `new R(out _)` at the top level / statement context
    // (`var q := new R(out _);`) recorded a scratch slot the function
    // never disposed: the produced owned heap value leaked (LSan Direct
    // leak in bz$R$ctor). Nested-in-a-method-call (`M(new R(out _))`)
    // is incidentally disposed by the enclosing lowerCall (the #1467
    // path), so this fix is strictly additive. The disposal runs at the
    // bottom of lowerNew, before each `return obj`, exactly mirroring
    // lowerCall's loop. // per §22.4, §28.3, §17.13, #1833 (b), #1467
    const std::size_t newDiscardOwnedBase = discardOwnedOutSlots_.size();
    // fix #2874 (§2.8.2 "No restrictions beyond visibility", v0.240): #2860's
    // "constructing imported generic ... cannot run the initializer of its
    // field" diagnostic is located from `importedFieldInitDiagLoc_`, but that
    // was set only on the no-explicit-ctor branch near the bottom of this
    // function. An imported generic WITH an explicit constructor reaches
    // `emitOwnFieldInit` through `inlineCtorBody` instead, so the diagnostic
    // came out UNLOCATED. MEASURED on 2ddd5b3, two real two-module builds of
    // the same uncarriable initializer: the no-ctor spelling reported
    // `app.bzl:5:20:` and the explicit-ctor spelling reported `app.bzl:0:0:` —
    // and an explicit constructor is the ordinary shape, so the located case
    // was the exception rather than the rule.
    //
    // Set it ONCE here, covering every construction path this function reaches,
    // and restore on the way out. `lowerNew` has many returns, hence the scope
    // guard rather than a paired assignment; the save/restore is what keeps a
    // nested `new Outer(new Inner())` attributing each field to its own `new`.
    struct ImportedFieldInitDiagLocScope {
        SourceLocation& slot;
        SourceLocation saved;
        ImportedFieldInitDiagLocScope(SourceLocation& s, SourceLocation v)
            : slot(s), saved(s) { slot = v; }
        ~ImportedFieldInitDiagLocScope() { slot = saved; }
    } importedFieldInitDiagLocScope(importedFieldInitDiagLoc_, ne.span.begin);
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
            sit->second->kind == sema::TypeRef::Kind::Named) {
            rt = const_cast<sema::TypeRef*>(sit->second);
            // #2017 / §14.2 (v0.197): a builtin-collection substitution
            // (`T = Array<int>`) satisfies `where T: new()` — it is a `public
            // class` with a public parameterless constructor (stdlib §4.4), and
            // §14.2 lists `Array<T>` among the class types — but it carries NO
            // user TypeSymbol (rt->declaration == null). The stdlib-collection /
            // builtin construction dispatch in the `!rt->declaration` block below
            // keys off `tnode`'s name; while `tnode` still named the bare
            // parameter `T`, `colName` was "T", every branch fell through, and
            // lowerNew returned nullptr — the local slot was never initialized,
            // so a later use dereferenced garbage (SIGSEGV). Redirect `tnode` to
            // the resolved substitution's node so `new T()` dispatches to the
            // real backing (bz_array_alloc, bz_dict_alloc, …). A class/record
            // substitution (declaration != null) skips the whole
            // `!rt->declaration` block, so `tnode` is unused there and left
            // untouched — that path is byte-identical to before.
            if (!rt->declaration && rt->astNode &&
                rt->astNode->name && !rt->astNode->name->parts.empty())
                tnode = rt->astNode;
        }
    }
    // fix #2477: `new T()` in the dead abstract template body — the
    // genericSubstSemaTy_ lookup just above is empty there (no monomorphization
    // binding) so the bare type-param TypeRef falls through every construction
    // branch below and lowerNew returns nullptr. The pre-existing #2434 generic
    // warning (lowerLocalDecl's zero-init fallback) still fires as a backstop;
    // this is the SPECIFIC, named complement that flags the abstract-template
    // shape by name so the silence→zero-init case is observable in source. The
    // abstract body is dead code (real calls route through the monomorphized
    // clone, per #2477's analysis), so this is purely a diagnostics-clarity
    // fix — no operational change. Gated on !hasErrors() to match the #2434
    // message-quality guard.
    if (!rt->declaration && rt->astNode && rt->astNode->name &&
        rt->astNode->name->parts.size() == 1 &&
        genericSubstSemaTy_.empty() &&
        templateGenericParamNames_.count(rt->astNode->name->parts.back()) &&
        !ctx_.diagnostics().hasErrors()) {
        ctx_.diagnostics().report(
            Severity::Warning, ne.span.begin,
            "internal: codegen cannot construct 'new " +
            std::string(rt->astNode->name->parts.back()) +
            "()' in the dead abstract template body: no monomorphization "
            "binding exists for this type parameter (spec #14.1 / #14.2 "
            "permits the form but the abstract body is never invoked — "
            "real calls route through the monomorphized clone — #2477)");
    }
    // §22.4 (#968): pre-create the `out var x` slot(s) for `new T(out var x)` so
    // the arg lowering below passes the slot's address and a later read sees the
    // value the constructor wrote (the call-site analogue of ensureOutVarSlots).
    ensureCtorOutVarSlots(ne, rt);
    if (!rt->declaration) {
        // §6.5 / §24 (#1646): user construction of a producer-side event source
        // — `new EventSource<T>()` / `new AsyncEventSource<T>()` (the constructors
        // are public as of stdlib §6.5, v0.187). These have no user TypeSymbol
        // (rt->declaration == null); their runtime value is a plain
        // bz_event_source_t* (isEventSourceTypeName / the event-as-value model),
        // the same object emitInstanceInit mints for a keyword-form event and that
        // Subscribe/Add/Remove/Raise/RaiseAsync dispatch against (tryLowerEventCall).
        // A fresh, empty source is allocated by bz_event_source_new (sync and async
        // share one runtime representation — RaiseAsync differs only in codegen).
        // Without this branch the construction fell through to `return nullptr`,
        // which ICE'd as a field initializer (#1386) and silently left a
        // ctor-assigned field null (Subscribe/Raise then no-op'd). The owned field
        // is freed via bz_event_source_free in emitOwnedFieldDisposals.
        if (isEventSourceTypeName(rt)) {
            auto* newFn = declareC("bz_event_source_new", ptrTy(), {});
            return builder_.CreateCall(newFn, {}, "event.source");
        }
        // fix #1832 / stdlib §6.5 (v0.187): user construction of a
        // cross-domain publication point — `new SharedEventSource<T>()`. Same
        // shape as the instance source above (declaration-less Named ref,
        // bz_event_source_t* runtime rep), with a distinct allocation entry
        // point bz_shared_event_source_new that mirrors the cross-domain
        // contract (synchronized quiescence on dispose; record-side the
        // runtime reserves a slot for a future mutex). The disposal arm in
        // emitOwnedFieldDisposals matches via isOwnedSharedEventSourceField.
        if (isSharedEventSourceTypeName(rt)) {
            auto* newFn = declareC("bz_shared_event_source_new", ptrTy(), {});
            return builder_.CreateCall(newFn, {}, "shared.event.source");
        }
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
                        // §31.1 / §28.9 (#1033): the exception OWNS its Message.
                        // A +1 producer (interpolation / `+` / slice / ToString)
                        // TRANSFERS its single reference into the slot (no
                        // retain); a BORROW initializer (literal / local / field
                        // / call) is RETAINED so the slot owns an independent
                        // +1 — mirroring lowerStringFieldStore. The +1 is
                        // released by the built-in exception disposer
                        // (bz$exDisposeMsg) on disposal; before this the +1
                        // from `new Exception($"...")` leaked (#1033).
                        if (!isPlusOneStringProducer(ne.args[0].value.get()))
                            emitStringRetain(msgStr);
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
                            // §31.1 / §28.9 (#1033): ParamName is an owned
                            // string slot, same retain/transfer model as the
                            // Message above (released by bz$exDisposeMsg).
                            if (!isPlusOneStringProducer(ne.args[1].value.get()))
                                emitStringRetain(pn);
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
            // §4.14 (#1275 + #3135): direct span constructors —
            //   new Span<T>(buffer, start, length)
            //   new ReadOnlySpan<T>(buffer, start, length)
            //   new ArraySpan<T>(source, start, length)
            //   new ReadOnlyArraySpan<T>(source, start, length)
            // These records lower to the { ptr source, i32 start, i32 length }
            // arrayspan struct (isSpanLikeTypeName). Constructing them via the
            // generic record path built a value-layout struct with i64
            // Start/Length, so the later i32 span reads picked up garbage and
            // indexed out of bounds (SIGSEGV). Lower the buffer/array arg to its
            // bz_array_t* backing and build the span directly.
            //
            // §4.14 "Construction patterns" (v0.195), verbatim: "The direct
            // constructors of the five span records do **not** range-check
            // `(Start, Length)` against the source: a mis-constructed span
            // faults at each out-of-range *access* instead — the span-level
            // check passes and the source's own bounds check throws
            // `IndexOutOfRangeException`". The safety of that composition is
            // owed by the indexer (lowerCollectionIndexerGet): it checks the
            // span's `Length` AND the source array's live `Count` (#1158), so
            // every out-of-source read still throws. The four array-backed
            // ctors matched this rule before #3135 only by accident of a
            // stale-line-number comment — #3098 added the matching
            // `ReadOnlyStringSpan` arm and surfaced the divergence. The eager
            // checks are removed here; the factories (`AsSpan`/
            // `AsReadOnlySpan`) and `Slice(r: Range<int>)` keep theirs and
            // validate eagerly, as §4.14 requires.
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
                return buildArraySpan(src, start, len);
            }
            // fix #3098 (language spec §17.7 "Standard-library builtins have
            // a closed constructor surface", v0.241; §31.4; stdlib §3.3): the
            // DECLARED positional primary constructors of the two RECORD-SHAPED
            // reserved builtins.
            //
            // §17.7, verbatim: "For a builtin record whose only declared
            // constructor is its positional primary constructor
            // (`ExceptionInfo`, §31.4; `ReadOnlyStringSpan`, stdlib §3.3), the
            // undeclared parameterless form `new T()` is rejected the same way
            // — exactly as for an identically-shaped user record." #2994
            // implemented that REJECTION; the same sentence KEEPS the positional
            // form, and `lowerNew` had no arm for it, so the kept spelling fell
            // through to `lowerLocalDecl`'s #2434 zero-init fallback: a record of
            // NULL strings behind an `internal: … #2434` note. MEASURED on
            // 663a68e, `new ExceptionInfo("TypeName","M","S")` then printed an
            // EMPTY line for `e.TypeName`, and reading the same field through a
            // concatenation — `"[" + e.TypeName + "]"` — exited 139
            // (SIGSEGV) on the null `bz_string*`. That is the #2922 class: a
            // silent wrong value escalating to a crash, on the spelling §31.4
            // and stdlib §3.3 themselves write (stdlib §3.3's own `Slice` is
            // defined as `new ReadOnlyStringSpan(Source, this.Start + start,
            // length)`).
            //
            // The reference behaviour §17.7 names is "an identically-shaped
            // user record", which works today, so both arms simply build the
            // value each type's LAYOUT already is: `ExceptionInfo` the
            // {ptr,ptr,ptr} of three `bz_string*` that `lowerTypeRef` gives it
            // and `lowerExceptionInfoField` reads back, `ReadOnlyStringSpan` the
            // shared {ptr,i32,i32} span struct (arraySpanStructType) that
            // `string.AsSpan()` already mints.
            //
            // ⚠ NO EAGER RANGE CHECK for `ReadOnlyStringSpan`, deliberately,
            // and unlike the four array-backed span constructors above: stdlib
            // §4.14 "Construction patterns" is explicit that "the direct
            // constructors of the five span records do **not** range-check
            // `(Start, Length)` against the source (v0.195): a mis-constructed
            // span faults at each out-of-range *access* instead". The indexer's
            // own `i < 0 or i >= Length` guard and the source string's bounds
            // check are what fault, exactly as §3.3 declares them.
            //
            // Named arguments are honoured by NAME rather than by position: a
            // record's positional constructor takes its field names as parameter
            // names (§9.1), so `new ExceptionInfo(Message := m, TypeName := t,
            // StackTrace := s)` — §22.5's named-argument spelling is
            // `name := value` — is the same value as the positional spelling.
            // Reading them positionally would be a SILENT field permutation —
            // the defect class this fix exists to remove — so an argument
            // naming nothing this record declares makes the arm DECLINE
            // (nullptr), which surfaces as the pre-existing located #2434 note
            // rather than as a wrong value.
            if (colName == "ExceptionInfo" || colName == "ReadOnlyStringSpan") {
                const bool isEI = (colName == "ExceptionInfo");
                static const char* const kEIFields[3] =
                    {"TypeName", "Message", "StackTrace"};      // §31.4
                static const char* const kRSFields[3] =
                    {"Source", "Start", "Length"};              // stdlib §3.3
                const char* const* fields = isEI ? kEIFields : kRSFields;
                if (ne.args.size() == 3) {
                    // Pass 1: resolve each WRITTEN argument to its field slot.
                    std::size_t slotOf[3] = {3, 3, 3};
                    bool claimed[3] = {false, false, false};
                    std::size_t pos = 0;
                    bool ok = true;
                    for (std::size_t ai = 0; ai < 3; ++ai) {
                        const auto& a = ne.args[ai];
                        if (!a.value) { ok = false; break; }
                        std::size_t slot = 3;
                        if (a.name.empty()) {
                            slot = pos++;
                        } else {
                            for (std::size_t j = 0; j < 3; ++j)
                                if (a.name == fields[j]) { slot = j; break; }
                        }
                        if (slot >= 3 || claimed[slot]) { ok = false; break; }
                        claimed[slot] = true;
                        slotOf[ai] = slot;
                    }
                    if (ok) {
                        // Pass 2: lower in SOURCE order (the order the arguments
                        // are WRITTEN), not in field order — §22.5 "Evaluation
                        // order": "A call's supplied argument expressions are
                        // evaluated in call-site textual order — left to right
                        // as written, for positional and named arguments alike;
                        // naming an argument changes which parameter it binds
                        // to, never when it runs" — and place each result in its
                        // own field slot.
                        llvm::Value* vals[3] = {nullptr, nullptr, nullptr};
                        for (std::size_t ai = 0; ai < 3; ++ai) {
                            llvm::Value* av = lowerExpr(*ne.args[ai].value);
                            if (!av) { ok = false; break; }
                            vals[slotOf[ai]] = av;
                        }
                        if (ok && vals[0] && vals[1] && vals[2]) {
                            if (isEI) {
                                auto* eiTy = llvm::StructType::get(
                                    llvm_, {ptrTy(), ptrTy(), ptrTy()});
                                llvm::Value* v = llvm::UndefValue::get(eiTy);
                                for (unsigned f = 0; f < 3; ++f)
                                    v = builder_.CreateInsertValue(
                                        v, vals[f], {f}, "excinfo.ctor");
                                return v;
                            }
                            // {ptr Source, i32 Start, i32 Length}: `int` lowers
                            // to i32 already, but a `long`-typed constant-narrowed
                            // argument can arrive wider — truncate as the
                            // span-ctor arm above does.
                            llvm::Value* st0 = vals[1];
                            llvm::Value* ln0 = vals[2];
                            if (!st0->getType()->isIntegerTy(32))
                                st0 = builder_.CreateTrunc(st0, i32(), "ross.st");
                            if (!ln0->getType()->isIntegerTy(32))
                                ln0 = builder_.CreateTrunc(ln0, i32(), "ross.len");
                            return buildArraySpan(vals[0], st0, ln0);
                        }
                    }
                }
                // Any other argument list is not a declared constructor of
                // either type. §17.7's own rejection of the parameterless form
                // is sema's (checkBuiltinClosedConstructorSurface, fix #2994);
                // declining here leaves every OTHER shape exactly where it was.
                return nullptr;
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
                // fix #2722 / stdlib §4.10 (v0.240): the comparer overloads.
                // §4.10 declares FOUR constructors on `Set<T>` (and the same
                // four on `OwningSet<T>`):
                //     public constructor();
                //     public constructor(capacity: int);
                //     public constructor(comparer: IEqualityComparer<T>);
                //     public constructor(capacity: int, comparer: IEqualityComparer<T>);
                // The two comparer-bearing forms were lowered by DROPPING the
                // comparer and building the table with the default content-based
                // trampolines, so a caller who supplied a comparer precisely
                // because default equality is wrong for their type silently got
                // default equality — a wrong answer on a documented API with no
                // diagnostic. Diagnose it instead, per the #511 `GetType`
                // precedent (a loud "not yet implemented" beats a silent wrong
                // value), and see rejectUnimplementedComparerCtor for why this
                // is a deferral rather than an implementation.
                // Return on rejection rather than falling through. A
                // class-element `OwningSet<T>` has a null `genericBackingType`,
                // so it falls out of this arm into the Array one, which reads
                // arg 0 as a capacity and emitted `sext ptr to i64` — malformed
                // IR ("SExt only operates on integer") stacked on top of the
                // real diagnostic. The compile already fails on the error; a
                // well-formed null keeps the verifier out of it.
                if (rejectUnimplementedComparerCtor(ne, colName))
                    return llvm::ConstantPointerNull::get(ptrTy());
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
                const sema::TypeRef* elemTy =
                    genericBackingType(rt, 0, /*isSet=*/true, &ksz);
                // fix #2773 (B1) / stdlib §4.18 (v0.286): `genericBackingType`
                // now admits a SortedSet<T> reference element (class,
                // interface) — see its own comment — but construction is
                // sound only when T actually has a DISPATCHABLE `CompareTo`;
                // `narrow #3428`'s loud rejection (below) stays in force for
                // the residual case (`Object`, or a class/interface that
                // declares none) rather than silently constructing a set
                // whose ordering and dedup cannot honor §4.18. Null this back
                // out so that residual case falls to the rejection exactly as
                // it did before this fix — a strict NARROWING of #3428's
                // rejection surface, not a new silent-wrong-value path (§6c).
                if (elemTy && colName == "SortedSet" &&
                    hasObjectHeaderEquality(elemTy) &&
                    !buildObjectCompareToThunk(elemTy)) {
                    elemTy = nullptr;
                }
                if (elemTy) {
                    llvm::Function* hashT = buildValueHashTrampoline(elemTy);
                    llvm::Function* eqT   = buildValueEqTrampoline(elemTy);
                    // fix #2773 (B1) / stdlib §4.18 (v0.286): a SortedSet's
                    // stored identity for a reference element is
                    // `CompareTo`-equality ("adding an equal element leaves
                    // the set unmodified"), not §8.3's `Equals`/
                    // `GetHashCode` — exactly the same divergence
                    // `fix #3488` recorded for `SortedDictionary`'s reference
                    // KEY, and the same fix: `buildSortedObjectKeyIdentity`
                    // overwrites hashT/eqT with the (constant-hash,
                    // `CompareTo == 0`) pair when the element carries a
                    // dispatchable `CompareTo`. Called unconditionally on
                    // `colName == "SortedSet"`, mirroring the
                    // `SortedDictionary` call site's own idiom: for a
                    // scalar/string/record/enum/choice element
                    // `buildObjectCompareToThunk` inside it returns null
                    // immediately (not `hasObjectHeaderEquality`), so hashT/
                    // eqT pass through unchanged and every non-reference
                    // SortedSet element is byte-identical to before this fix.
                    if (colName == "SortedSet")
                        buildSortedObjectKeyIdentity(elemTy, hashT, eqT);
                    auto* allocFn = declareC("bz_dict_alloc_generic", ptrTy(),
                                             {i64(), ptrTy(), ptrTy()});
                    return builder_.CreateCall(allocFn,
                        {builder_.getInt64((int64_t)ksz), hashT, eqT});
                }
                // narrow #3428 / stdlib §4.18 (v0.286): the residual case —
                // a `SortedSet<T>` REFERENCE element (class, interface,
                // `Object`) with NO dispatchable `CompareTo`. This used to be
                // EVERY reference element (#3428's original filing, v0.283);
                // fix #2773 (B1) narrowed it to just this shape once
                // `buildObjectCompareToThunk` (fix #3488's lift of the
                // #1195/#2768 `Array.Sort` thunks) gave every OTHER
                // reference element a real comparator, handled by the
                // `elemTy` branch above. `SortedSet` is NOT in the Array
                // arm's name list below, so falling out of both branches
                // here still means `lowerNew` would return nullptr and the
                // user would see lowerLocalDecl's #2974 degrade — kept LOUD
                // instead, per the #511 `GetType` / #2722 comparer-
                // constructor precedent, exactly as #3428 chose.
                //
                // The predicate is `hasObjectHeaderEquality` — the same named
                // authority `genericBackingType`'s dictionary row uses to
                // decide "carries the §8.3 object header: a class, an
                // interface, or `Object`" — rather than a second spelling of
                // the same classification.
                if (colName == "SortedSet" && rt->genericArgs.size() == 1) {
                    const sema::TypeRef* rejElemTy =
                        resolveTypeParamRef(rt->genericArgs[0]);
                    if (hasObjectHeaderEquality(rejElemTy)) {
                        const bool isObj =
                            rejElemTy->kind == sema::TypeRef::Kind::Primitive;
                        std::string elemName =
                            isObj ? "Object" : std::string(rejElemTy->headName());
                        const bool isIface =
                            !isObj && rejElemTy->declaration->kind ==
                                          ast::TypeDeclKind::Interface;
                        const char* kindWord =
                            isObj ? "the `Object` type"
                                  : (isIface ? "an interface type"
                                             : "a class type");
                        // The element is named by its HEAD (`headName()`,
                        // the alias-transparent authority) rather than a
                        // reconstructed full spelling: there is no shared
                        // ast::Type printer in this TU, and the three that
                        // exist are file-local statics in other TUs. So the
                        // message describes the head instead of presenting
                        // `SortedSet<IComparable>` as if that were what the
                        // program wrote for `SortedSet<IComparable<Ord>>`.

                        // review fix (post-0c8612d): NULLABILITY is checked
                        // FIRST and given its OWN message, rather than being
                        // folded into the class/interface "no dispatchable
                        // CompareTo" wording below. `genericBackingType`'s
                        // pre-existing `!ty->nullable` guard
                        // (LlvmBackend_Calls3.cpp, "Nullable keys and other
                        // shapes are not wired here") excludes EVERY
                        // nullable key/element from the generic-key backing
                        // BEFORE any `CompareTo` is ever consulted — so
                        // `SortedSet<Ord?>` with `Ord: IComparable<Ord>` (a
                        // REAL, dispatchable `CompareTo`) reached this same
                        // block under the OLD wording and was told "`Ord`
                        // declares no single-argument `CompareTo`", which is
                        // FALSE: `Ord` declares exactly one — the nullable
                        // wrapper is what this code path never gets past.
                        // The accept/reject BOUNDARY here is still correct
                        // (spec §14.2: `where T: IComparable<T>` is a
                        // type-bound constraint, and such a constraint
                        // implies a non-null `T` — a nullable `T` cannot
                        // itself conform to an interface), only the STATED
                        // reason was wrong for this shape. Not folded into
                        // the "no CompareTo" wording below because that
                        // wording's own consequence clause ("membership
                        // would fall back to reference identity") describes
                        // what a WRONG comparator dispatch would do — the
                        // nullable case never reaches comparator dispatch at
                        // all, so that clause would not even apply.
                        if (rejElemTy->nullable) {
                            ctx_.diagnostics().report(
                                Severity::Error, ne.span.begin,
                                "stdlib §4.18: `SortedSet<T>` requires a "
                                "NON-NULLABLE element type — the element "
                                "type here (`" + elemName + "?`) is " +
                                kindWord + ", used nullably. Spec §14.2's "
                                "`where T: IComparable<T>` type-bound "
                                "constraint implies a non-null T (a "
                                "nullable T cannot itself conform to an "
                                "interface), and this compiler's "
                                "generic-key backing does not admit a "
                                "nullable class/interface element/key at "
                                "all regardless of whether the underlying "
                                "type declares `CompareTo`. Use a "
                                "non-nullable element type, or use `Set<T>` "
                                "(stdlib §4.10) where ordering is not "
                                "required (#3428).");
                            return llvm::ConstantPointerNull::get(ptrTy());
                        }
                        // The INTERFACE wording is deliberately NOT "declares
                        // no CompareTo": an interface reaching here can
                        // perfectly well declare one — `IComparable<Ord>`
                        // used bare as an element type declares
                        // `CompareTo(other: Ord): int` — but
                        // `selfComparableIfaceCompareTo` requires the
                        // SELF-comparable shape §4.18's `where T:
                        // IComparable<T>` names (`CompareTo` comparing T
                        // against T itself), which that is not. Saying "no
                        // CompareTo" for that shape would be a false claim
                        // about the program's own source.
                        ctx_.diagnostics().report(
                            Severity::Error, ne.span.begin,
                            "stdlib §4.18: `SortedSet<T>` requires an "
                            "element type with a dispatchable `CompareTo` — "
                            "the element type here (`" + elemName + "`) is " +
                            kindWord + ", and " +
                            (isObj
                                 ? std::string("`Object` declares no ")
                             : isIface
                                 ? (std::string("`") + elemName +
                                    "` does not declare a SELF-comparable ")
                                 : (std::string("`") + elemName +
                                    "` declares no single-argument ")) +
                            "`CompareTo`. Constructing one would silently "
                            "break both halves of §4.18's contract "
                            "(\"membership by `CompareTo`-equality, "
                            "iteration in ascending order\"): membership "
                            "would fall back to §8.3 reference identity, so "
                            "two otherwise-equal instances would both be "
                            "stored, and iteration would come out in hash "
                            "order. Implement `CompareTo(other: T): int` "
                            "(stdlib §2.8's `IComparable<T>`), or use "
                            "`Set<T>` (stdlib §4.10) where ordering is not "
                            "required (#3428).");
                        return llvm::ConstantPointerNull::get(ptrTy());
                    }
                }
            }
            // per §4.6/§4.7: LinkedList/OwningLinkedList use the real doubly-linked-
            // list runtime (bz_llist_alloc), not the bz_array_t backing. The
            // elem_size argument tells the runtime how many bytes to store inline
            // in each node — same sizing as bz_array_t (4 for int/float, 8 otherwise).
            if (colName == "LinkedList" || colName == "OwningLinkedList") {
                auto* allocFn = declareC("bz_llist_alloc", ptrTy(), {i64()});
                auto allocEmpty = [&]() -> llvm::Value* {
                    return builder_.CreateCall(allocFn,
                                               {builder_.getInt64(elemSize)});
                };
                if (ne.args.empty() || !ne.args[0].value) return allocEmpty();
                // fix #2693 / stdlib §4.6 (v0.240): the SOURCE constructor.
                // §4.6 declares exactly two constructors on `LinkedList<T>`:
                //   public constructor();
                //   public constructor(source: IEnumerable<T>);
                // and stdlib §5 recommends the second by name ("use `new
                // LinkedList<T>(source)` if a linked list is wanted"). Codegen
                // never read `ne.args`, so the second silently produced an EMPTY
                // list — a wrong value on a documented API with no diagnostic.
                //
                // §4.7 declares only `constructor()` on `OwningLinkedList<T>`,
                // so the source form is NOT specified there and is REJECTED
                // rather than invented: taking elements out of an `IEnumerable<T>`
                // into an owning list would have to either alias (two owners →
                // double dispose) or consume the borrowed source, and the stdlib
                // document answers neither. Same for a second argument on either
                // type — no such constructor exists.
                if (colName == "OwningLinkedList" || ne.args.size() != 1) {
                    ctx_.diagnostics().report(
                        Severity::Error, ne.span.begin,
                        "no such constructor: stdlib " +
                        std::string(colName == "OwningLinkedList" ? "§4.7"
                                                                  : "§4.6") +
                        " declares " +
                        std::string(colName == "OwningLinkedList"
                                        ? "only 'constructor()' on "
                                          "'OwningLinkedList<T>'"
                                        : "'constructor()' and "
                                          "'constructor(source: IEnumerable<T>)' "
                                          "on 'LinkedList<T>'"));
                    return allocEmpty();
                }
                // The source is BORROWED (`source: IEnumerable<T>` carries no
                // `owned` marker, spec §22.1). lowerToLazyEnumerator is the one
                // backing-agnostic normalizer — it covers Array/OwningArray/
                // Stack/Queue/OwningBuffer (bz_array_t*), LinkedList
                // (bz_lazy_from_llist), Set/Dictionary, Range, a lazy LINQ
                // pipeline, IGrouping, and a vtable-backed IEnumerable<T>.
                //
                // ⚠ We dispose the source NOWHERE, which is why no double
                // dispose is possible. The "dispose only if it was a temporary"
                // rule lives ONLY in lowerToLazyEnumerator's vtable path
                // (materializeVtableIEnumerableToArray); the array path is a
                // pure borrow (bz_lazy_from_array) that delegates temp-reclaim
                // to the caller through its `outRecvArr` out-param, which we do
                // not pass. Any temp-source leak here is inherited verbatim from
                // the ToArray convention, not introduced by this arm.
                sema::TypeRef* srcElemRef = nullptr;
                llvm::Value* lazy =
                    lowerToLazyEnumerator(*ne.args[0].value, srcElemRef);
                // Element-type guard, mirroring OwningSet.ExceptWith's: reading
                // the snapshot at the list's stride is only sound when the
                // source's element lowers to the same LLVM type. Sema has no
                // constructor model for the builtin collections (they have no
                // TypeSymbol), so an ill-typed `new LinkedList<int>(42)` reaches
                // codegen — diagnose it here instead of reading at a wrong
                // stride or falling back to the silently-empty list.
                if (!lazy || !srcElemRef || rt->genericArgs.empty() ||
                    !rt->genericArgs[0] ||
                    lowerTypeRef(srcElemRef) != lowerTypeRef(rt->genericArgs[0])) {
                    ctx_.diagnostics().report(
                        Severity::Error, ne.span.begin,
                        "stdlib §4.6: 'new LinkedList<T>(source)' requires an "
                        "enumerable source whose element type is T");
                    return allocEmpty();
                }
                // Drain the pipeline once into a snapshot array, then append in
                // source order. §4.6 fixes the orientation for the sibling
                // literal form ("textual order is head-to-tail"); the source
                // form follows the same convention, so element 0 of the source
                // becomes `First` and the last becomes `Last`.
                auto* toArrFn =
                    declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
                llvm::Value* snap = builder_.CreateCall(
                    toArrFn, {lazy, builder_.getInt64(elemSize)}, "ll.ctor.snap");
                llvm::Value* list = allocEmpty();
                // fix #2727 (§28.9 / §28.3, v0.240): a fresh-minting lazy source
                // (any LINQ operator that mints heap strings, e.g. `.Select($"…")`)
                // yields elements with rc=1 that the snapshot "inherits" from the
                // pipeline — the pipeline takes no reference on its end
                // (`bz_lazy_to_array` does not retain, per the existing ToArray
                // convention). For a borrowed source this +1 belongs to the
                // upstream collection and is released by it at scope end; for a
                // fresh-minting source the snapshot is the SOLE owner of that +1
                // and MUST release it, or the pipeline's "+1 per element" leaks
                // (#2727's measured rc = 2 - 1 list release = 1 leftover).
                //
                // The fix mirrors what ToArray already does: retain all string
                // elements on the snapshot (bz_array_retain_strings) so the
                // snapshot EXPLICITLY owns +1 per element, then release that
                // explicit +1 by freeing the snapshot with the string disposer
                // (stringElementReleaseThunk) instead of a NULL disposer. For a
                // borrowed source this is balanced by the source's own release
                // (the source kept its independent +1), so total = +3 - 3 = 0.
                // For a fresh-minting source this consumes the snapshot's +1,
                // and the pipeline's "+1" is released by bz_lazy_dispose's own
                // path on the materialized/owned array it drained.
                const ElementPolicy ctorPol =
                    elementPolicyFor(rt->genericArgs[0], /*owningFamily=*/false);
                const bool ctorRetainStr =
                    rt->genericArgs[0]->kind == sema::TypeRef::Kind::Primitive &&
                    rt->genericArgs[0]->primitive == sema::PrimitiveType::String &&
                    !rt->genericArgs[0]->nullable &&
                    ctorPol.cat == ElementPolicy::Category::RuntimeManaged;
                if (ctorRetainStr) {
                    auto* retainFn = declareC("bz_array_retain_strings",
                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
                    // void return — name is illegal (LLVM would reject the IR);
                    // see also the free call below.
                    builder_.CreateCall(retainFn, {snap});
                }
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                       {ptrTy(), i64(), i64()});
                auto* addLastFn = declareC("bz_llist_add_last", ptrTy(),
                                           {ptrTy(), ptrTy(), i64()});
                llvm::Value* snapLen =
                    builder_.CreateCall(lenFn, {snap}, "ll.ctor.n");
                auto* idxSlot = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), idxSlot);
                auto* cBB = llvm::BasicBlock::Create(llvm_, "ll.ctor.c", currentFn_);
                auto* bBB = llvm::BasicBlock::Create(llvm_, "ll.ctor.b", currentFn_);
                auto* eBB = llvm::BasicBlock::Create(llvm_, "ll.ctor.e", currentFn_);
                builder_.CreateBr(cBB);
                builder_.SetInsertPoint(cBB);
                llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot, "ll.ctor.i");
                builder_.CreateCondBr(
                    builder_.CreateICmpSLT(idx, snapLen), bBB, eBB);
                builder_.SetInsertPoint(bBB);
                {
                    llvm::Value* ep = builder_.CreateCall(
                        ptrFn, {snap, idx, builder_.getInt64(elemSize)},
                        "ll.ctor.ep");
                    if (ctorRetainStr) {
                        llvm::Value* sv =
                            builder_.CreateLoad(ptrTy(), ep, "ll.ctor.s");
                        emitStringRetain(sv);
                    }
                    // bz_llist_add_last memcpy's elemSize bytes FROM ep, so the
                    // snapshot slot address can be handed over directly.
                    builder_.CreateCall(
                        addLastFn, {list, ep, builder_.getInt64(elemSize)});
                    builder_.CreateStore(
                        builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
                    builder_.CreateBr(cBB);
                }
                builder_.SetInsertPoint(eBB);
                // fix #2727 (§28.9 / §28.3, v0.240): the snapshot explicitly
                // retains a +1 per string element (above) and so MUST release
                // that +1 at free time — a NULL disposer (the "consumed snapshot"
                // idiom ExceptWith/IntersectWith use) leaks the snapshot's own
                // +1 for every fresh-minting element. A borrowed source still
                // balances: the source keeps its independent +1 and releases it
                // at scope end, so the total is +3 (source +1, snapshot retain
                // +1, list retain +1) − 3 (snapshot release, list release, source
                // release) = 0.
                auto* freeSnapFn = declareC("bz_array_free",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                llvm::Value* snapDisp = ctorRetainStr
                    ? llvm::cast<llvm::Value>(stringElementReleaseThunk())
                    : llvm::ConstantPointerNull::get(ptrTy());
                builder_.CreateCall(freeSnapFn, {snap, snapDisp});
                return list;
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
                // #2608 / §28.11 / stdlib §4.3: record the element disposer of an
                // OWNING array-backed collection in its header at CONSTRUCTION.
                // Element ownership is otherwise a purely static property, chosen
                // at the free site from the collection's static type. That fails
                // for an `owned` field typed by a read-only interface SHARED by
                // both hierarchies (IReadOnlyList<T>/IReadOnlyCollection<T> are
                // common ancestors of Array<T> AND OwningArray<T> per stdlib
                // §4.3), where the same static type can carry either backing and
                // the free site cannot tell which. The concrete type IS known
                // here, so stamp the disposer now; bz_array_free_dynamic reads it
                // back at the erased free site. Borrowing collections are never
                // tagged (NULL = today's behavior), so this cannot over-dispose.
                // #2635: the stamp itself now lives in the shared
                // stampOwningElemDisposer member (LlvmBackend_impl.hpp) rather
                // than in this lambda, because two OTHER construction sites
                // call bz_array_alloc directly and were silently skipping it
                // (materializeHeapCollectionLiteral /
                // materializeArrayBackedSetLiteral). This lambda is now just
                // the value-passthrough wrapper this arm's `return` style needs.
                auto tagOwningElems = [&](llvm::Value* arr) -> llvm::Value* {
                    stampOwningElemDisposer(
                        arr,
                        rt->genericArgs.empty() ? nullptr : rt->genericArgs[0],
                        common::isOwningPrefixedName(colName));
                    return arr;
                };
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
                        return tagOwningElems(builder_.CreateCall(allocFn,
                            {builder_.getInt64(elemSize), cap}, "col.cap.init"));
                    }
                }
                return tagOwningElems(builder_.CreateCall(allocFn,
                    {builder_.getInt64(elemSize), builder_.getInt64(16)}));
            }
            if (colName == "Dictionary" || colName == "OwningDictionary" ||
                // stdlib §4.17 (#1354, #1406): SortedDictionary<K,V> shares
                // the bz_dict_t* runtime representation (same hash-table backing
                // as Dictionary). Without this branch lowerNew returned nullptr,
                // leaving the variable slot uninitialized — bz_dict_* calls then
                // received a zero/i32 receiver and produced malformed IR.
                colName == "SortedDictionary") {
                // fix #2722 / stdlib §4.8 (v0.240): the comparer overloads.
                // §4.8 declares on `Dictionary<K, V>` (and §4.9 the same pair on
                // `OwningDictionary<K, V>`):
                //     public constructor(comparer: IEqualityComparer<K>);
                //     public constructor(capacity: int, comparer: IEqualityComparer<K>);
                // Both were lowered by DROPPING the comparer — see the §4.10
                // note in the Set arm above; §4.8's "Stability while stored"
                // paragraph names "the collection's `IEqualityComparer` (hash
                // forms)" as governing lookup, so the comparer is normative
                // rather than advisory and silently ignoring it is a wrong
                // answer, not a liberty.
                // Return on rejection rather than falling through. A
                // class-element `OwningSet<T>` has a null `genericBackingType`,
                // so it falls out of this arm into the Array one, which reads
                // arg 0 as a capacity and emitted `sext ptr to i64` — malformed
                // IR ("SExt only operates on integer") stacked on top of the
                // real diagnostic. The compile already fails on the error; a
                // well-formed null keeps the verifier out of it.
                if (rejectUnimplementedComparerCtor(ne, colName))
                    return llvm::ConstantPointerNull::get(ptrTy());
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
                    // fix #3488 / stdlib §4.17 (v0.283): a SORTED map's stored
                    // identity is `CompareTo`-equality ("Two keys are the same
                    // key exactly when `CompareTo` returns `0`"), not §8.3's
                    // `Equals`/`GetHashCode`. For a scalar/string/record/enum
                    // key the two coincide and nothing changes; for a REFERENCE
                    // key they do not, and the §8.3 pair stored two
                    // `CompareTo`-equal keys as distinct. Only
                    // `SortedDictionary` is retargeted — `Dictionary` /
                    // `OwningDictionary` keep §8.3 reference identity, which is
                    // what §4.8 / §4.9 specify for them.
                    if (colName == "SortedDictionary")
                        buildSortedObjectKeyIdentity(keyTy, hashT, eqT);
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
                // fix #4884 (stdlib §1/§3.1, v0.307): "Negative magnitudes fault"
                // requires capacity < 0 to raise ArgumentOutOfRangeException
                // WITHOUT allocating the backing buffer. The guard must therefore
                // run BEFORE bz_array_alloc below, mirroring the ordering already
                // used by the sibling sized constructors OwningBuffer<T>(n)
                // (~L6498) and Array<T>(size, fill) (~L6610) — previously it ran
                // after the alloc, so a throwing negative-capacity call leaked the
                // 56-byte header + 512-byte 64-slot backing store (568 B / 2
                // allocations) on every call, since the ctor never returns a
                // value for a caller to dispose.
                bool isStrArg = false;
                if (!ne.args.empty() && ne.args[0].value) {
                    auto* argTy = ctx_.exprType(*ne.args[0].value);
                    isStrArg = argTy &&
                        argTy->kind == sema::TypeRef::Kind::Primitive &&
                        argTy->primitive == sema::PrimitiveType::String;
                    if (!isStrArg && argTy &&
                        argTy->kind == sema::TypeRef::Kind::Primitive &&
                        argTy->primitive == sema::PrimitiveType::Int &&
                        !argTy->nullable) {
                        // stdlib §1/§3.1 (v0.183, #1396): StringBuilder(capacity)
                        // faults on a negative capacity. The capacity value is
                        // not otherwise used to size the backing buffer below
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
                auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                llvm::Value* sb = builder_.CreateCall(allocFn,
                    {builder_.getInt64(SB_ESZ), builder_.getInt64(64)});
                // Fix #531 (§3.1): new StringBuilder(initial: string) — if a string
                // arg is present, append it as the first part via bz_array_add,
                // reusing the same IR sequence as Append(string) in Calls.cpp.
                // The no-arg ctor and capacity-int ctor are not affected: the
                // capacity arg has an int type (not String), so isStrArg is false.
                if (isStrArg) {
                    const ast::Expr* sExpr = ne.args[0].value.get();
                    llvm::Value* s = lowerExpr(*ne.args[0].value);
                    if (s) {
                        // #1185 / stdlib §3.1: the builder owns a +1 for the
                        // initial part — retain a borrow, TRANSFER a +1
                        // producer (mirrors Append in LlvmBackend_Calls.cpp).
                        if (!isPlusOneStringProducer(sExpr))
                            emitStringRetain(s);
                        llvm::AllocaInst* slot = entryAlloca(ptrTy());
                        builder_.CreateStore(s, slot);
                        auto* addFn = declareC("bz_array_add",
                                               llvm::Type::getVoidTy(llvm_),
                                               {ptrTy(), ptrTy(), i64()});
                        builder_.CreateCall(addFn,
                            {sb, slot, builder_.getInt64(SB_ESZ)});
                    }
                }
                return sb;
            }
            if (colName == "Random") {
                if (!ne.args.empty() && ne.args[0].value) {
                    llvm::Value* sv = lowerExpr(*ne.args[0].value);
                    // §5.1 v0.254 / stdlib §2.7 / #3154: the constructor
                    // parameter is `seed: long`, so an int-typed argument
                    // (int literal, int local, int expression) must be
                    // sign-extended to i64 before the `bz_random_alloc(i64)`
                    // call. The earlier code checked the type and discarded
                    // the outcome (assigned a never-read local), so a wider
                    // `i32` value was passed where `i64` was expected and the
                    // LLVM module verifier rejected the call.
                    if (sv && !sv->getType()->isIntegerTy(64))
                        sv = builder_.CreateSExt(sv, i64());
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
                llvm::Value* r = builder_.CreateCall(newFn, {val, dispArg},
                                                     "shared.new");
                // fix #1970 / §28.4 / §28.6: `new Shared<T>(owned v)` (§28.13)
                // takes ownership of `v` (the disposer thunk or builtin
                // disposer is stored in the control block; on the last strong
                // release it disposes `v`). Null the caller's slot for a bare
                // owned local/param argument so the caller's null-guarded
                // unwindOnly disposal — which the walker now leaves eligible
                // for this NewExpr shape via newExprRoutesThroughNullingCtor's
                // builtin Shared head-name branch — is exactly-once: disposes a
                // value still live at a throw that PRECEDES the ctor (fixes
                // the #1970 leak); skips the already-stored value at the
                // post-ctor throw edge (no double-free with Shared's
                // disposer on the last-release path, the standard §28.13
                // dispose-once invariant). A `new Shared<T>(move x)` / `new
                // Shared<T>(producer())` source shape is unaffected
                // (nullTransferredOwnedLocalSlot's own eligibility test
                // restricts to a plain owned local / owned parameter).
                nullTransferredOwnedLocalSlot(ne.args[0].value.get());
                return r;
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
                        // fix #4445 (stdlib §6.3, v0.302): guard the two-arg
                        // constructor's ArgumentException contract —
                        //   max < 1 → ArgumentException
                        //   initial > max → ArgumentException
                        // (The one-arg form defaults max to INT_MAX, so both
                        // conditions are inert and never reach here.) Thrown
                        // BEFORE allocation, so no semaphore leaks on the
                        // guard path. Mirrors the Channel(capacity) guard
                        // (v0.174 / #809, just above in this block).
                        emitGuardThrow(
                            builder_.CreateOr(
                                builder_.CreateICmpSLT(maxCount,
                                                       builder_.getInt32(1),
                                                       "sem.max.bad"),
                                builder_.CreateICmpSGT(initCount, maxCount,
                                                       "sem.init.bad"),
                                "sem.bad"),
                            "ArgumentException");
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
                        // #2618 / §7.4: a flow-narrowed `T?` initial value
                        // (proved non-null by a preceding null-check) is
                        // admitted by sema as the non-nullable `initial: T`
                        // parameter (stdlib §6.4) but keeps its tagged-optional
                        // {i8 hasVal; T payload} layout. Unwrap to the bare T
                        // payload before the widen ladder below — otherwise the
                        // struct fails every isIntegerTy() branch, `initVal`
                        // stays getInt64(0), and the cell is SILENTLY
                        // initialized to 0 (a wrong value, not a verifyModule
                        // ICE). Same root cause and same one-line unwrap as the
                        // atomic instance-method sites fixed in #2605
                        // (LlvmBackend_Calls3.cpp / LlvmBackend_Stmts.cpp);
                        // unwrapNarrowedNullable is a no-op for a plain
                        // primitive, so every existing plain-T `new Atomic<T>(k)`
                        // is unchanged.
                        if (newTR && !newTR->genericArgs.empty() &&
                            newTR->genericArgs[0])
                            if (auto* tTy = lowerTypeRef(newTR->genericArgs[0]))
                                raw = unwrapNarrowedNullable(raw, tTy);
                        // fix #2634 / §7.4 / §5.1 (v0.236): the unwrap above
                        // requires the box payload to EQUAL T's LLVM type, so it
                        // covers only `T? -> Atomic<T>`. A narrowed `int?` into
                        // `Atomic<long>` has payload i32 against T's i64: no
                        // branch fires, the `{i8,i32}` struct then matches no
                        // isIntegerTy() test below, and `initVal` keeps its
                        // getInt64(0) default — the #2618 symptom again, at a
                        // WIDENING argument type. §5.1 admits that argument
                        // ("Widening from a signed type to a strictly larger
                        // signed type"), and `new Atomic<long>(someInt)`
                        // compiles today, so the narrowed form is reachable.
                        //
                        // Unwrap by the box's OWN payload type instead, letting
                        // the payload feed the widen ladder below rather than
                        // bypass it.
                        //
                        // SOUNDNESS — this is gated on the ARGUMENT's sema type
                        // being a nullable primitive, which is EXACTLY the
                        // trigger condition of the #2618 sema gate
                        // (ExprTypecheck_Exprs.cpp ~2639). That gate checks the
                        // argument against the non-nullable form of its OWN
                        // type, so it fires for `int?` regardless of what T is;
                        // verified mechanically that `new Atomic<long>(n)` with
                        // an un-narrowed `int? n` reports "the argument to
                        // `Atomic` constructor is nullable but the target type
                        // is non-nullable (§7)". So wherever this unwrap fires,
                        // sema has PROVED the box non-null and the payload read
                        // is safe. Keying on the sema type rather than on the
                        // struct shape alone is deliberate: the constructor
                        // argument is otherwise UNTYPED-CHECKED here (#2618
                        // declined to re-implement ctor argument typing), so a
                        // bare shape test could strip a field out of some other
                        // 2-element aggregate that sema never vetted.
                        const sema::TypeRef* argSemaTy =
                            ctx_.exprType(*ne.args[0].value);
                        const bool argIsPrim =
                            argSemaTy &&
                            argSemaTy->kind == sema::TypeRef::Kind::Primitive;
                        if (argIsPrim && argSemaTy->nullable)
                            if (auto* st =
                                    llvm::dyn_cast<llvm::StructType>(
                                        raw->getType()))
                                if (st->getNumElements() == 2 &&
                                    st->getElementType(0) ==
                                        nullablePrimHasValTy() &&
                                    st->getElementType(1)->isIntegerTy())
                                    raw = unwrapNarrowedNullable(
                                        raw, st->getElementType(1));
                        // Widen to i64 for bz_atomic_new's uint64_t parameter.
                        //
                        // fix #2634 / §5.1: widen by SIGNEDNESS. This ladder
                        // zero-extended unconditionally, so every signed source
                        // NARROWER than the cell lost its sign — proven, not
                        // inferred: `new Atomic<long>(x)` with `x: int := -1`
                        // read back 4294967295, and `new Atomic<int>(x)` with
                        // `x: sbyte := sbyte(-1)` read back 255. That half is
                        // PRE-EXISTING (it needs no nullable at all), but it
                        // must be fixed here: the unwrap above is what lets a
                        // narrowed `int?` reach this widen, so shipping the
                        // unwrap alone would turn "silently 0" into "silently
                        // sign-flipped" for a negative payload. Same pairing,
                        // and same isUnsignedPrim key (LlvmBackend_impl.hpp
                        // ~3066), as the sibling site fixed in #2617.
                        //
                        // The i1 branch stays FIRST and stays a ZExt: `bool` is
                        // not in isUnsignedPrim's set, so routing it through the
                        // signedness branch would SExt `true` to all-ones.
                        //
                        // Fallback is the previous ZExt whenever the source is
                        // not positively known to be a signed primitive (sema
                        // type absent, or a non-primitive such as IntPtr), so
                        // behaviour changes ONLY where signedness is proven.
                        if (raw->getType()->isIntegerTy(1)) {
                            initVal = builder_.CreateZExt(raw, i64());
                        } else if (raw->getType()->isIntegerTy() &&
                                   !raw->getType()->isIntegerTy(64)) {
                            const bool srcSigned =
                                argIsPrim &&
                                !isUnsignedPrim(argSemaTy->primitive);
                            initVal = srcSigned
                                          ? builder_.CreateSExt(raw, i64())
                                          : builder_.CreateZExt(raw, i64());
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
            // stdlib §7.2 / issue #645: the two declared FileStream constructors
            //   constructor(path: string, mode: FileMode)
            //   constructor(path: string, mode: FileMode, access: FileAccess)
            // lower to bz_filestream_open(path_data, path_len, mode, access).
            // The enum arguments are ordinary Bezel enum values (the prelude
            // declares them `: byte`), so widen whatever integer width they
            // lower to up to the runtime's i32.
            //
            // NOTE (recorded choice, #645): stdlib §7.2 declares the two-argument
            // constructor but does NOT state which FileAccess it implies. This
            // lowering uses FileAccess.ReadWrite (ordinal 2) — every mode except
            // `Open` creates or rewrites the file, so a read-only default would
            // make five of the six modes unusable through the short form. The
            // spec gap is filed separately; a later ruling changes only this
            // constant.
            if (colName == "FileStream" &&
                (ne.args.size() == 2 || ne.args.size() == 3) &&
                ne.args[0].value && ne.args[1].value) {
                llvm::Value* pathStr = lowerExpr(*ne.args[0].value);
                if (!pathStr) return nullptr;
                llvm::Value* pathData = loadStringData(pathStr);
                llvm::Value* pathLen  = loadStringLen(pathStr);
                auto toI32 = [&](llvm::Value* v) -> llvm::Value* {
                    if (!v) return nullptr;
                    if (v->getType()->isIntegerTy(32)) return v;
                    if (v->getType()->isIntegerTy())
                        return builder_.CreateZExt(v, i32());
                    return nullptr;
                };
                llvm::Value* mode = toI32(lowerExpr(*ne.args[1].value));
                if (!mode) return nullptr;
                llvm::Value* access = builder_.getInt32(2);  // FileAccess.ReadWrite
                if (ne.args.size() == 3 && ne.args[2].value) {
                    access = toI32(lowerExpr(*ne.args[2].value));
                    if (!access) return nullptr;
                }
                auto* openFn = declareC("bz_filestream_open", ptrTy(),
                                        {ptrTy(), i64(), i32(), i32()});
                return builder_.CreateCall(
                    openFn, {pathData, pathLen, mode, access}, "fs.open");
            }
            // fix #2974 (language spec §11.7 "Static classes", stdlib §6.8,
            // v0.240): THE `new Domain(name: string)` ARM IS DELETED HERE.
            //
            // It emitted `bz_domain_alloc` for a construction the standard
            // library does not declare. stdlib §6.8 is `public static class
            // Domain` with twelve `Spawn` overloads and NO constructor and NO
            // `Name`; §11.7 is verbatim "A static class may not be instantiated:
            // `new` on a static class is a compile error", and adds "Because a
            // static class cannot be instantiated, it does not implement
            // `IDisposable`" — so `using var d := new Domain(…)` was doubly
            // ill-formed. The arm's own runtime comment records how it got here:
            // "stdlib §6.8 documents Domain as a static-class for Domain.Spawn.
            // The probe suite exercises `new Domain("test")` and `d.Name` as an
            // instance-creation path…". A probe wanted a capability, the
            // compiler grew it undeclared, and four tests pinned it under a
            // `// §6.8` citation for a section that declares none of it.
            //
            // WHY DELETION IS THE FIX AND A GUARD IS NOT. #2974 read the
            // acceptance as a missing check ("does this built-in name denote an
            // instantiable class?"). It was not a gap: with the arm present,
            // `new Domain("d")` PRODUCED A VALUE and so looked exactly like a
            // working construction to every downstream check — which is why it
            // ran, and why it leaked 714 bytes a call (nothing releases a
            // built-in the auto-drop chain has no arm for). With the arm gone it
            // produces no value like every other non-constructible built-in, and
            // the ONE property in `lowerLocalDecl`/the return site
            // (`unconstructedBuiltinNewName`) rejects it together with the seven
            // others that were already silently degrading — `Path`, `File`,
            // `Directory`, `Debug`, `BitConverter`, `Encoding`, `Linq`. One
            // property, not a `Domain` special case.
            //
            // §6.8's REAL surface — `Domain.Spawn` — is a static call on the
            // identifier and lives in LlvmBackend_Calls3.cpp; it is untouched.
            // fix #2993 (language spec §11.7 "Static classes"; stdlib §6.8
            // `Domain`): the companion `.Name` getter (LlvmBackend_Exprs.cpp)
            // and `Domain` arm of the explicit `Dispose()` chain
            // (LlvmBackend_Calls2.cpp) — the "instance" surface the stdlib does
            // not declare — are removed in the same fix, together with the
            // runtime trio (`bz_domain_alloc`/`bz_domain_get_name`/
            // `bz_domain_dispose` and `bz_domain_t::name`) that existed only to
            // back them. §11.7's "static class cannot be instantiated, does not
            // implement `IDisposable`" is now load-bearing for both spellings.
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
    //
    // fix #3143 (§14.1 / §2.5, v0.254): ONE branch, reading the parameter names
    // from whichever source carries them. This used to be two — a local
    // (`t->decl`) arm and #428's separate imported arm — and they had DRIFTED:
    // only the local arm applied #732's `allUnbound` guard, so
    // `new ImportedBox<T>()` inside a generic method's (dead, unspecialized)
    // TEMPLATE body installed a T→i32 substitution and produced a
    // `%record.Box$spec0` value in a function whose declared return type was
    // emitted as the unspecialized `%record.Box`. One predicate now, so the two
    // cannot disagree again.
    {
        const std::vector<std::string_view> gpNames =
            rt->genericArgs.empty() ? std::vector<std::string_view>{}
                                    : sema::genericParamNames(t);
        if (!gpNames.empty() && gpNames.size() == rt->genericArgs.size()) {
            // #732: if EVERY generic arg is an UNBOUND type parameter (a generic
            // METHOD template body, e.g. `new Box<T>` inside `Make<T>` where T
            // is not bound in the active subst), do NOT install a substitution.
            // Installing one would lowerTypeRef(T) → the i32 fallback and
            // specialize the record (`record.Box$spec0`), but the function's
            // declared return type `Box<T>` was emitted UNspecialized
            // (`record.Box`, via recordStruct's matching allUnbound path), so the
            // produced value would mismatch the return type → IR-verify failure.
            // Mirrors the allUnbound test in lowerTypeRef's generic-RECORD
            // branch (LlvmBackend_Layout.cpp) -- corrected citation: the
            // pre-#3143 comment pointed at :594-609, which is a different
            // function entirely. The two must agree, because that branch
            // returning the UNSPECIALIZED struct while this one installs a
            // substitution is exactly the IR mismatch #3143's T5 measured.
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
            // #428 / §14.1 / §2.5: this covers the IMPORTED generic type too —
            // no ast::TypeDecl is available (decl is null for imported
            // TypeSymbols), but the parameter names are in `t->genericParams`
            // (GenericParamSymbol*, populated by
            // ModuleLoader::convertGenericParams), which is where
            // `sema::genericParamNames` reads them. Without the substitution
            // recordStruct/classStruct laid the T-typed fields out at the i32
            // placeholder for a non-int type argument.
            if (!allUnbound) {
                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
                for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                    llvm::Type* concrTy = lowerTypeRef(rt->genericArgs[gi]);
                    std::string_view pname = gpNames[gi];
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

    // fix #2889 (§14.1 "Generic declarations" / §12 "Inheritance and
    // Dispatch" / §22.8 "Field initializers", v0.240): a GENERIC class over a
    // GENERIC ancestor needs the ANCESTOR's parameter names bound too, and it
    // needs them bound for the WHOLE construction, not just for the layout
    // query below.
    //
    // `classStruct` composes these bindings transiently for its own layout
    // (so a field read / disposer / method body gets the right offsets from
    // wherever it is called), but §22.8's construction sequence runs many more
    // steps against the same object — the implicit base chain, each ancestor's
    // field initializers, the inlined base constructor bodies. Those recurse
    // level by level and hand each level's IMMEDIATE derived class to
    // `computeAncestorGenericSubst`; at a middle link (`L<X> : M<W> : B<T>`)
    // the receiver is `M`, whose parameter `W` the `new` site's substitution
    // does not name, so the chain went unresolved and the base's initializers
    // stored at the erased T→i32 offsets into a correctly sized object —
    // measured: `cell := new Cell()` 8 bytes early, `l.cell.tag` SEGV.
    //
    // Composing the whole chain ONCE here, into the substitution `lowerNew`
    // already keeps active until after the constructor runs (and already
    // restores on every exit), makes every one of those steps resolvable
    // without threading a most-derived receiver through the recursion.
    // A no-op for a non-generic `t`, a `t` with no generic ancestor, and a
    // `class D<T> : B<T>` whose base already shares the name.
    mergeAncestorGenericSubst(t);

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
    //   - the importer emits: calloc → store primary-ctor positional args into
    //     their field slots (fix #3201: §22.8 step 0) → install vtable → done.
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
    // fix #2319 / §22.3: accessor-mediated `ref`/`out` argument bracket
    // (copy-IN) recorded by the pre-pass; consumed by the ctor-call section
    // below to flush the copy-OUT after the ctor body runs. Empty unless an
    // indexer-slot / property bracket was emitted — `ref local`/`ref field`
    // still resolve through lvalueAddr and never enter here.
    std::vector<AccessorRefArg>    prebuiltAccessorRefArgs;

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
        // fix #5213 (§22.5 / §22.6, v0.313; §2.5): the constructor this `new`
        // runs is the §22.6 winner sema selected (`ctorForArgs` adopts
        // `SemaContext::resolvedNewCtor`), not the first declared constructor
        // that carries a body — with several constructors the first-declared
        // pick bound the wrong parameter list, failed to build the arguments,
        // and ran no constructor body at all, silently.
        if (auto* pick = ctorForArgs(t, ne.args);
            pick && !pick->isStatic && pick->decl != nullptr)
            importedGenericExplicitCs = pick;
        if (!importedGenericExplicitCs) {
            for (sema::Symbol* m : t->members) {
                auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
                if (cs && !cs->isStatic && cs->decl != nullptr) {
                    importedGenericExplicitCs = cs;
                    break;
                }
            }
        }
    }
    const bool importedGenericWithBody =
        importedGenericExplicitCs != nullptr;
    // fix #3713 (§22.8 step 2 / §22.10 / §2.8.3 rule 4, v0.286): the
    // DIRECT-PRIMARY `new` shape — a class that declares BOTH a §22.10 primary
    // constructor and at least one explicit constructor, constructed through
    // the primary because no explicit constructor matched the call's arguments.
    // #3625's sema fix (`checkNewExprArgs`'s fall-through to
    // `checkPositionalNewArgsFromSymbols`) admitted this shape; before it the
    // construction was rejected outright, which is why no codegen path handles
    // §22.8 step 2 for it. Both arms below read this ONE predicate so the local
    // and imported paths cannot disagree (§2.8.3 rule 4).
    //
    // Each conjunct is load-bearing:
    //   - `Class` + `hasPositionalFields`: the class HAS a primary constructor.
    //     `hasPositionalFields` is the both-sources flag (`Parser_Decls.cpp`
    //     sets it the moment the header's `(` is seen, so an empty primary
    //     parameter list counts; `ModuleMetadataBuilder` carries it across the
    //     wire) — the SAME gate `storePrimaryCtorPositionalArgs` uses for step 0
    //     one screen down, not a second spelling of it.
    //   - `hasAnyConstructor(t)`: without an explicit constructor the existing
    //     no-explicit-ctor path already runs steps 1+2 (locally through the
    //     `else if (!hasAnyConstructor(t))` branch, cross-module through the
    //     exporter's `bz$<Type>$init`). Admitting those here would DOUBLE-RUN
    //     every field initializer — §22.8's "field initializers run exactly once
    //     per instance construction".
    //   - `ctorForArgs(t, ne.args) == nullptr`: an explicit constructor that DID
    //     match runs steps 1+2 inside its own compiled body (locally via
    //     `inlineCtorBody`, cross-module via `bz$<Type>$ctor`), so this is the
    //     same once-only guard from the other side.
    const bool directPrimaryNew =
        t->kind == ast::TypeDeclKind::Class && t->hasPositionalFields &&
        hasAnyConstructor(t) && ctorForArgs(t, ne.args) == nullptr;
    if (ctx_.isImported(t) && !importedGeneric) {
        // fix #3162 (§22.10 / §22.8 step 0 / §2.8.3 rule 4, v0.254): a
        // dependency's `bz$<Type>$init` takes only the object, so a §22.10
        // primary constructor's PARAMETERS have no channel across the module
        // boundary and `new ImportedPBox(3, 4)` was rejected outright (#3141's
        // gate) rather than silently producing a zero object.
        //
        // No new ABI is needed, because step 0 is the IMPORTER's job anyway.
        // §22.8 orders construction: step 0 stores the primary parameters into
        // their field slots, step 1 runs the base chain, step 2 runs the field
        // initializers. `bz$<Type>$init` is exactly steps 1+2 for a
        // no-explicit-ctor class (emitInstanceInitFunction: emitInstanceInit +
        // emitImplicitBase + emitOwnFieldInit) — and it runs them against the
        // object it is HANDED. So storing the arguments here, before the call,
        // puts them in place in the order §22.8 requires: a dependency-side
        // initializer that READS a primary parameter (`public Q: int := V * 2;`)
        // reads the value stored a moment ago, not the calloc zero.
        //
        // The slot indices are the importer's own `fieldIndex_` over
        // `classStruct(t)`, the same layout the dependency's `init` GEPs
        // through (§2.8.3 rule 4: ONE runtime type identity), and the same
        // helper the LOCAL arm below uses — see its declaration comment.
        //
        // This is the direct-`new` twin of the base-clause path #3141 fixed:
        // `class D(): PBox(5, 6)` already stores the base's positional fields
        // at the derived `new` site and then routes through `bz$<Base>$init`
        // (`emitImportedBaseFieldInit`, LlvmBackend_Setup.cpp). Same sequence,
        // one level down.
        //
        // `currentThis_` is deliberately NOT set to `obj` here (unlike the
        // local arm): nothing in this arm resolves `t`'s OWN implicit-field
        // reads — the dependency's initializers run in the dependency's own
        // compiled body — so the enclosing `this` is what an argument
        // expression needs, and it is already current (#2367).
        //
        // `!hasAnyConstructor(t)` is the SAME shape #3141's sema gate rejects
        // and this fix enables, and no other. An imported class that declares
        // an explicit constructor ALONGSIDE its primary one already
        // constructs through `bz$<Type>$ctor` below with `ne.args` bound to
        // THAT constructor's parameters; storing those same arguments into the
        // primary-parameter fields as well would be a second, unasked-for
        // write. (The LOCAL arm does exactly that today — the §22.10 combined
        // form is deferred there, #3163 — but widening the imported arm to
        // match a deferred local shape is not this fix's business, and keeping
        // the guard leaves every imported shape that works today bit-identical.)
        //
        // fix #3625 codegen half (§22.8 step 0 / §22.10 / §2.8.3 rule 4, v0.286):
        // the `!hasAnyConstructor(t)` gate ABOVE was the bug. With the gate,
        // an imported class with BOTH a primary ctor AND an explicit
        // convenience ctor, called via the (2) shape — `new WithConv(7,
        // "direct")` against `class WithConv(a: int, b: string) +
        // constructor(x: int) this(x, "conv")` — bound sema to the PRIMARY
        // (sema's #3625 W3-L1 fix's fall-through arm), so `ctorForArgs(t,
        // ne.args)` returned null (the primary has no ConstructorSymbol —
        // only the convenience ctor does, and its arity is 1). With no
        // matching ctor, the imported-ctor call block at line 6411 emitted
        // the #2658 "no constructor call could be lowered" warning and
        // returned `obj` without ever populating the field slots, so the
        // instance kept its calloc-zero values (`a=0`, `b=""`) — a
        // §22.10/§22.5 silent wrong value, the codegen half of #3625.
        //
        // The gate's "second, unasked-for write" concern was unfounded. For
        // the (1) shape — `new WithConv(6)` binding to the convenience ctor —
        // the explicit `bz$<Type>$ctor` body inlines the chain call
        // `this(x, "conv")`, which in turn inlines the primary ctor's body
        // (the same `storePrimaryCtorPositionalArgs` helper, now run against
        // the chain args). The chain's writes overwrite this importer-side
        // step 0 with the SAME primary-field values, so the importer-side
        // write is wasted but never incorrect — exactly the posture the
        // LOCAL arm above already takes (#3583). For the (2) shape, the
        // importer-side step 0 is the ONLY place the primary ctor's body
        // runs, and removing the gate is what makes it run.
        //
        // The gate is removed for `hasAnyConstructor(t)` only. The
        // no-explicit-ctor path was always correct (the gate let it through,
        // and the pre-existing #3162 behavior is unchanged). The shape this
        // widens to is exactly one family: an imported non-generic class
        // whose explicit ctor DOES NOT match the call's arity (so the
        // convenience-bound path at line 6411 does not fire) AND whose
        // primary ctor arity DOES match (so sema binds to primary via the
        // §22.5/§22.10 fall-through in `checkNewExprArgs`). Sema's published
        // `ctx_.positionalBinding(ne)` is the primary-vs-arg pairing in that
        // case, so `storePrimaryCtorPositionalArgs` writes the right values.
        //
        // The follow-up shape — an imported class with BOTH primary ctor AND
        // an explicit ctor AND a field initializer (`class WithInit(a: int,
        // b: string) { public x: int := 42; public constructor(x: int)
        // this(x, "conv"); }`) — still misses step 2 (field initializer) for
        // the (2) shape because `bz$<T>$init` is vtable-only for a
        // has-explicit-ctor class (#3162 measured this same gap on the local
        // side and #3163 deferred the wider fix). Filed as #3649; not in
        // this commit's scope (the issue's measured repro has no field
        // initializer, so step 0 is the only thing the codegen-side needs to
        // restore).
        //
        // The `!hasAnyConstructor(t)` gate IS REMOVED: step 0 now runs for
        // has-explicit-ctor imported classes too. The gate's "second,
        // unasked-for write" concern was unfounded. For the (1) shape —
        // `new WithConv(6)` binding to the convenience ctor — the explicit
        // `bz$<Type>$ctor` body inlines the chain call `this(x, "conv")`,
        // which in turn inlines the primary ctor's body (the same
        // `storePrimaryCtorPositionalArgs` helper, now run against the chain
        // args). The chain's writes overwrite this importer-side step 0 with
        // the SAME primary-field values, so the importer-side write is wasted
        // but never incorrect — exactly the posture the LOCAL arm above
        // already takes (#3583 unconditionally runs step 0 for any
        // non-chains-via-this call, which is true for both shapes). For the
        // (2) shape, the importer-side step 0 is the ONLY place the primary
        // ctor's body runs, and removing the gate is what makes it run.
        //
        // The shape this widens to is exactly one family: an imported
        // non-generic class whose explicit ctor DOES NOT match the call's
        // arity (so the convenience-bound path at line 6411 does not fire)
        // AND whose primary ctor arity DOES match (so sema binds to primary
        // via the §22.5/§22.10 fall-through in `checkNewExprArgs`). Sema's
        // published `ctx_.positionalBinding(ne)` is the primary-vs-arg
        // pairing in that case, so `storePrimaryCtorPositionalArgs` writes
        // the right values. The no-explicit-ctor path was already correct
        // (the gate let it through, and the pre-existing #3162 behavior is
        // unchanged).
        storePrimaryCtorPositionalArgs(
            t, obj, st, ne.args, ctx_.positionalBinding(ne), savedSubst,
            savedSemaTy, currentThis_);
        llvm::Function* initFn = declareImportedInit(t);
        builder_.CreateCall(initFn, {obj});
        // fix #3713 (§22.8 step 1+2 / §22.10 / §2.8.3 rule 4, v0.286): the
        // IMPORTED half of the direct-primary gap. `bz$<Type>$init` above is
        // vtable-only for a class that declares an explicit constructor
        // (`emitInstanceInitFunction`'s `!hasAnyConstructor(ts)` gate) — the
        // design assumes the importer ALSO calls `bz$<Type>$ctor`, whose
        // compiled body runs steps 1+2. On this shape no `bz$<Type>$ctor` is
        // ever called (the primary constructor has no `ConstructorSymbol`), so
        // step 1 and step 2 ran NOWHERE and every field outside the primary
        // parameter list kept its §7.9 default. Measured on r12-cycle-1:
        // `public class WithCounter(a: int, b: string)` with
        // `public x: int := WithCounter.Bump();` and a convenience ctor, called
        // as `new WithCounter(7, "direct")`, printed `x = 0` and
        // `WithCounter.Bumps() = 0` — the counter proving the initializer
        // EXPRESSION never evaluated, not merely that a field read zero.
        //
        // The importer cannot run step 2 itself: `emitOwnFieldInit` walks
        // `ast::FieldDecl` initializers off each member's `decl`, always null
        // for an imported symbol (the #2374-class gap), and an initializer may
        // legitimately name the exporter's PRIVATE statics (`_bumps` above), so
        // there is nothing here to re-lower even in principle. The exporter is
        // the only side that can run it, exactly as #2452 concluded for an
        // imported no-explicit-ctor BASE (`emitImportedBaseFieldInit` routes
        // through `bz$<Base>$init` for the same reason). `bz$<Type>$initprimary`
        // is that entry point for this shape: steps 1+2, no vtable store (the
        // `$init` call above already made it), run against the object it is
        // handed — with the primary parameters already in their slots from the
        // step-0 store immediately above, so an initializer that READS a primary
        // parameter sees the call's argument and not the calloc zero (§22.8's
        // step 0 → 1 → 2 order, preserved across the boundary).
        if (directPrimaryNew)
            builder_.CreateCall(primaryInitFn(t), {obj});
    } else if (importedGenericNoBody || importedGenericWithBody) {
        // Foundation A: no explicit ctor — install vtable, no ctor body.
        // Foundation C: explicit ctor with loaded body — install vtable first,
        // then fall through to the inline-ctor path (hasAnyConstructor guard).
        //
        // fix #3201 (§22.10 / §22.8 step 0 / §14.1 / §2.8.3 rule 4, v0.254):
        // the GENERIC half of #3162 — `lowerNew` for an imported generic has
        // no `bz$<Type>$init` to call (a generic is monomorphized in the
        // importer, per §14.1), so it INSTALLS the per-instantiation vtable
        // via `emitInstanceInit` and STOPPED there. With no step 0, the
        // primary-constructor parameters were DROPPED and the object left
        // calloc-zero — measured for `class GBox<T>(V: int, W: int)`,
        // `new GBox<int>(3, 4)` answered `sum=0 v=0 w=0` (the silent-zero
        // defect #3141 declined rather than ship). The fix is the same shape
        // the non-generic imported arm uses: store the primary-constructor
        // arguments into the FIELD SLOTS here, BEFORE `emitInstanceInit`
        // installs the vtable — the importer's callsite is the storage for
        // the primary parameters, the same layout the dependency's vtable's
        // disposer and the importer's later field reads GEP through
        // (§2.8.3 rule 4: one runtime type identity).
        //
        // `storePrimaryCtorPositionalArgs` was extracted by #3162 for exactly
        // this kind of reuse, and it already handles the generic's
        // `genericSubst_` (the LOCAL arm at line 5317 and the importer-side
        // non-generic arm at line 5285 use the same helper, with the same
        // SIGNATURE — the substitution is passed through unchanged).
        // `currentThis_` is NOT set to `obj` here: nothing in this arm
        // resolves `t`'s own implicit-field reads (the importer has no field
        // initializers to walk for an imported generic — `emitInstanceInit`
        // for an imported generic only installs the vtable and returns, see
        // LlvmBackend_Setup.cpp:9680), so the enclosing `this` is what an
        // argument expression needs and it is already current (#2367).
        //
        // The same `!hasAnyConstructor(t)` guard the non-generic arm uses
        // applies here: an imported generic WITH an explicit body (the
        // Foundation C arm, `importedGenericWithBody`) compiles a body that
        // BUILDS the field values from its own parameters, so a step 0
        // store would be a duplicate write. `#662` keeps the loaded body
        // exhaustive only for the no-ctor case in the metadata format, so
        // `compatibility-check.sh` does not measure this duplication today;
        // the guard is the same shape, the same asymmetry, and the same
        // reason as the non-generic imported arm one section up.
        if (!hasAnyConstructor(t))
            storePrimaryCtorPositionalArgs(
                t, obj, st, ne.args, ctx_.positionalBinding(ne), savedSubst,
                savedSemaTy, currentThis_);
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
        // fix #3141 (family F3 of umbrella #3152, §22.8 step 0): the primary-
        // constructor parameter list is the indexed symbol-side view
        // (`recordPositionalFieldSymbols`), which is the same list `td->
        // positionalFields` names — a local positional FieldSymbol's `posParam`
        // points into it — reached so that the pairing of argument i with field
        // i does not depend on an `ast::TypeDecl`. It also replaces the O(n²)
        // `posParam == &pf` scan over `members` this used to do per argument.
        // The flag is checked FIRST so the vector is built only for a
        // primary-constructor class, not on every `new`.
        // fix #3141 / #3162 (§22.8 step 0): store the primary-constructor
        // arguments into their field slots before emitInstanceInit runs the
        // field initializers, which may read them. The IMPORTED arm above
        // calls the same helper before `bz$<Type>$init`.
        //
        // fix #3583 (§22.8 / §22.10, v0.286): ...but NOT when this `new` binds
        // to an explicit §22.10 convenience constructor. §22.8:
        //
        //   "A constructor that chains via `this(...)` **skips steps 0 and 2**:
        //    its effective sequence is the chained `this(...)` call, then its
        //    own body. (Step 0 is vacuous for such a constructor anyway: only
        //    non-primary constructors may chain via `this(...)` (§22.7,
        //    §22.10), and primary-parameter capture happens inside the
        //    chained-to primary.)"
        //
        // The arguments of such a call bind to the CONVENIENCE constructor's
        // parameter list, which this helper does not describe: it pairs
        // argument i with positional field i (or with the §22.5 binding sema
        // published for the PRIMARY's parameter list, which is not the list
        // these arguments were bound against). Running it here therefore stored
        // a `string` argument into an `int` slot and vice versa — measured on
        // 907ce0f, `class B(n: int, s: string)` + `constructor(w: string,
        // v: int) this(s := w, n := v);` + `new B("hi", 3)` printed `4423024`
        // and then SIGSEGVed (issue 3562). The store is not merely wrong, it is
        // redundant: the chained-to primary's step 0 — `emitChainCall`'s
        // `this(...)`-to-primary arm, which since this fix calls THIS SAME
        // HELPER — writes every one of those slots before anything reads them.
        //
        // The predicate is §22.8's own: does the constructor this call binds to
        // chain via `this(...)`? In a class with a primary constructor
        // `Resolver::checkTypeConstructorChains` (fix #3535) makes every
        // accepted explicit constructor do exactly that, so this coincides with
        // the cheaper `!ctorForArgs(t, ne.args)` on every well-formed program;
        // spelling it as the spec does keeps it right for a class whose
        // constructor does NOT chain (a non-primary class, where the helper
        // already returns early on `!hasPositionalFields`).
        const bool boundCtorChainsThis = ctorChainsViaThis(ctorForArgs(t, ne.args));
        if (!boundCtorChainsThis)
            storePrimaryCtorPositionalArgs(
                t, obj, st, ne.args, ctx_.positionalBinding(ne), savedSubst,
                savedSemaTy, savedThisForInit);
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
            // fix #3501 (§4.7 / §7.10 / §14.1, v0.283): keep the constructed
            // class's UNIT bindings visible while its ctor arguments are
            // lowered, so `buildOrderedArgs`'s §4.7 rescue can see that the
            // formal `u: T` of `new B<()>(C.Nothing())` IS the unit type. See
            // `carryCalleeUnitSubst`. Restored with the rest below.
            carryCalleeUnitSubst(ctorArgSemaSave);
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
                    // #1955 / §28.3: null the caller's slot for a bare owned
                    // local/param transferred via `owned x` into an `owned` ctor
                    // parameter, so the caller's null-guarded unwindOnly disposal
                    // (which UnwindIneligibleWalker now leaves eligible for this
                    // shape) stays exactly-once across the transfer.
                    // #1968 / §28.3: reclaim the uninitialized `obj` if a ctor
                    // ARGUMENT throws during evaluation (obj is calloc'd here but
                    // the ctor has not run). Route arg-eval throws through a raw-
                    // free cleanup, restoring the enclosing target before the ctor.
                    llvm::BasicBlock* savedArgUT = unwindTarget_;
                    if (!ne.args.empty()) unwindTarget_ = newArgUnwindCleanup(obj);
                    // fix #2319 / §22.3: pass an accessor-ref-arg sink so
                    // buildOrderedArgs can emit the §22.3 copy-IN bracket for
                    // an indexer-slot / property `ref`/`out` argument here.
                    // The pre-pass already lowered the arg expression; the
                    // stored records (and their pinned receiver/index values)
                    // are consumed by the ctor-call section below, which
                    // emits the setter (the copy-OUT half) once the ctor body
                    // has returned NORMALLY. Same plumbing as lowerCall's
                    // method-call path; the helpers themselves are unchanged.
                    std::vector<AccessorRefArg> accessorRefArgs;
                    const std::size_t bareTBase = bareTParamNarrowUnwraps_.size();
                    bool ok = buildOrderedArgs(earlyCd->params, ne.args, eArgs,
                                              &eArgExprs,
                                              /*nullTransferredOwned=*/true,
                                              &accessorRefArgs);
                    // fix #4945 (§14.1 / §7.10, v0.307): restore bare-`T`
                    // nullable carriers under the constructed type's subst.
                    restoreCtorBareTParamCarriers(earlyCd->params, bareTBase,
                                                  eArgs, ctorArgSemaSave);
                    unwindTarget_ = savedArgUT;
                    currentThis_ = savedThisForArgs;
                    if (ok) {
                        // Store for the ctor-call section below to reuse.
                        prebuiltCtorArgs  = std::move(eArgs);
                        prebuiltCtorExprs = std::move(eArgExprs);
                        prebuiltCtorCs    = earlyCs;
                        prebuiltCtorFn    = earlyIt->second;
                        // fix #2319: forward the §22.3 bracket records so the
                        // ctor-call section can flush the setter after the body
                        // runs. The prebuilt args are reused verbatim below
                        // (no re-lowering), so the bracket setup moves with them.
                        prebuiltAccessorRefArgs = std::move(accessorRefArgs);
                        // Pre-populate base positional-param fields using
                        // args[1..n] (param values, not the `this` in args[0]).
                        std::vector<llvm::Value*> paramVals(
                            prebuiltCtorArgs.begin() + 1,
                            prebuiltCtorArgs.end());
                        // fix #2584: these args follow the CG50 by-ref ABI
                        // (a ref/out param's arg is the caller's storage
                        // ADDRESS), so the pre-pass must bind ref/out params
                        // through refParams_, not paramSlots_.
                        //
                        // fix #3583 (§22.8 / §22.10, v0.286): skipped for a
                        // `this(...)`-chaining constructor, for the same reason
                        // step 0 is. The §22.10 inheritance-clause base call
                        // (`class B(n: int, s: string): A(n)`) is evaluated
                        // against the object's OWN positional slots — case (a)
                        // of `prePopulateBasePositionalFields` — so running it
                        // here would read slots the chained-to primary has not
                        // written yet, and store the calloc zero into the base.
                        // The chain arm now runs it immediately after its step 0
                        // (LlvmBackend_Setup.cpp), where the slots hold their
                        // §22.8-ordered values. This is the third consumer of
                        // the blind store issue 3583 records; without moving it,
                        // `class A(p: int)` + `class B(n: int, s: string): A(n)`
                        // + `constructor(v: int) this(v, "conv");` printed
                        // `8 / conv / 0` instead of `8 / conv / 8`.
                        if (!boundCtorChainsThis)
                            prePopulateBasePositionalFields(
                                t, obj, paramVals, earlyCs,
                                /*ctorArgsByRefAbi=*/true);
                    } else {
                        // fix #2319: the call never runs, so no §22.3 write-back
                        // is owed — release the pinned receiver/index values.
                        emitAccessorRefArgWriteBacks(accessorRefArgs, /*emit=*/false);
                    }
                }
            }
            if (prebuiltCtorArgs.empty() && !boundCtorChainsThis) {
                // No explicit ctor (primary-ctor class with primaryBaseCtorArgs
                // or no chain at all): still pre-populate from primaryBaseCtorArgs.
                // (a): primaryBaseCtorArgs expressions reference primary-ctor
                // FieldSymbols on `t` → evaluated via implicitFieldAddr(GEP obj),
                // so currentThis_ = obj is already correct here.
                // fix #3583: `!boundCtorChainsThis` covers the recovery path
                // where the bound convenience constructor was found but its
                // arguments did not build (`ok == false` above) — the chain arm
                // still owns the base pre-population for it.
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
            // #1955 / §28.3: null the caller's slot for a bare owned local/param
            // transferred via `owned x` into an `owned` ctor parameter (see the
            // early-ctor path above and buildOrderedArgs).
            // #1968 / §28.3: reclaim the uninitialized `obj` if a ctor argument
            // throws during evaluation (obj is calloc'd + vtable-installed but the
            // ctor body has not run — every field is zero). Restored before the
            // inlined ctor body, whose own cleanup then owns the block.
            llvm::BasicBlock* savedArgUT = unwindTarget_;
            if (!ne.args.empty()) unwindTarget_ = newArgUnwindCleanup(obj);
            // fix #2319 / §22.3: pass an accessor-ref-arg sink so buildOrderedArgs
            // emits the §22.3 copy-IN bracket for an indexer-slot / property
            // `ref`/`out` argument. The setter (copy-OUT) is flushed below,
            // after the inlined ctor body returns NORMALLY — the exception edge
            // skips the write-back per §22.3's "on an exception exit no write-
            // back occurs". Mirrors lowerCall's method-call path; helpers unchanged.
            std::vector<AccessorRefArg> accessorRefArgs;
            const std::size_t bareTBase = bareTParamNarrowUnwraps_.size();
            bool builtArgs = buildOrderedArgs(cd->params, ne.args, args,
                                              &ctorArgExprs,
                                              /*nullTransferredOwned=*/true,
                                              &accessorRefArgs);
            // fix #4945 (§14.1 / §7.10, v0.307): see restoreCtorBareTParamCarriers.
            restoreCtorBareTParamCarriers(cd->params, bareTBase, args,
                                          ctorArgSemaTy);
            unwindTarget_ = savedArgUT;
            genericSubst_ = std::move(ctorArgSubst);
            genericSubstSemaTy_ = std::move(ctorArgSemaTy);
            if (builtArgs && args.size() == cs->params.size() + 1) {
                llvm::Value* savedThis = currentThis_;
                auto savedLocals = locals_;
                auto savedParams = paramSlots_;
                auto savedRefParams = refParams_;  // fix #2283
                llvm::BasicBlock* savedUnwindTarget = unwindTarget_;
                std::size_t savedUnwindDepth = unwindScopeDepth_;
                currentThis_ = obj;
                locals_.clear();
                paramSlots_.clear();
                // Bind ctor params to the lowered arg values.
                // fix #2283 / §22.3: a `ref`/`out` param's arg is the caller's
                // storage ADDRESS (alias path via lvalueAddr) or the §22.3
                // copy-in slot (bracket path) — register it in refParams_ so
                // the inlined body's reads/writes dereference through it,
                // exactly as bindParameters does for a compiled ctor's ref
                // param. Binding it in paramSlots_ instead left the body
                // loading the pointer itself (`add ptr, i32` — malformed IR).
                for (std::size_t pi = 0; pi < cs->params.size(); ++pi) {
                    auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                        cs->params[pi]);
                    if (ps && isByRefMod(ps->modifier)) {
                        llvm::Type* elemTy =
                            ps->declType ? lowerType(ps->declType) : i32();
                        refParams_[cs->params[pi]] = {args[pi + 1], elemTy};
                    } else {
                        auto* slot = entryAlloca(args[pi + 1]->getType());
                        builder_.CreateStore(args[pi + 1], slot);
                        paramSlots_[cs->params[pi]] = slot;
                    }
                }
                inlineCtorBody(cs, obj);
                unwindTarget_ = savedUnwindTarget;
                unwindScopeDepth_ = savedUnwindDepth;
                currentThis_ = savedThis;
                locals_ = std::move(savedLocals);
                paramSlots_ = std::move(savedParams);
                refParams_ = std::move(savedRefParams);
                // fix #2319 / §22.3: flush the copy-OUT half of every
                // accessor-mediated `ref`/`out` argument. The body has now
                // written the slot's final value (or thrown — `!diverged_`
                // matches §22.3's exception edge rule and lowerCall's own
                // call-site discipline exactly). Mirrors the post-call
                // emitAccessorRefArgWriteBacks call in lowerCall.
                emitAccessorRefArgWriteBacks(accessorRefArgs,
                                             /*emit=*/!diverged_);
                // Release any +1 string producer ctor args after body completes.
                if (!diverged_) {
                    for (std::size_t ai = 1;
                         ai < args.size() && ai < ctorArgExprs.size() + 1; ++ai) {
                        if (ctorArgExprs[ai - 1] &&
                            isPlusOneStringProducer(ctorArgExprs[ai - 1]))
                            emitStringRelease(args[ai]);
                    }
                }
            } else {
                // fix #2319: the ctor body never runs (buildOrderedArgs failed
                // or args size mismatched) — release the pinned receiver/index
                // values so their refcount isn't stranded.
                emitAccessorRefArgWriteBacks(accessorRefArgs, /*emit=*/false);
                // fix #5213 (§22.5 / §22.6, v0.313): never leave this silent.
                // Sema accepted the call, so a constructor whose arguments
                // cannot be built here would otherwise produce an instance
                // whose constructor body never ran, with no diagnostic.
                if (!diverged_ && !ctx_.diagnostics().hasErrors())
                    ctx_.diagnostics().report(
                        Severity::Error, ne.span.begin,
                        "internal: cannot lower this `new` of an imported "
                        "generic class — its constructor arguments could not "
                        "be bound to the selected constructor, so the "
                        "constructor body would not run (§22.5, §22.6, "
                        "#5213)");
            }
        }
        emitNewPendingCheck(obj);
        // §22.16 (v0.240, #2941): construction exit — evaluate the class
        // invariant of the constructed type AFTER the ctor call returns
        // normally and BEFORE the `obj` reference is exposed to the caller
        // (§22.16 "Every fresh instance satisfies its full dynamic class
        // invariant before it is observable").
        //
        // fix #4592 (§22.16 / §2.8.2, v0.306): this comment used to say that
        // emitInvariantCheck is "a no-op for imported types — their invariants
        // were already checked at construction time in the exporter". Both
        // halves were false. §22.16 puts the construction-exit check at the
        // most-derived constructor's completion, which codegen emits HERE, at
        // the `new` site — so a `new` in this module is a point the exporter
        // never compiled, and the imported chain link was simply dropped.
        // `emitInvariantCheck` now reads the loader's re-parsed clause
        // expressions for an imported class. Skipped when the ctor threw (the
        // emitNewPendingCheck above branched to the unwind edge). The
        // `disposeAndFreeOnThrow = true` flag wires the construction-exit
        // unwind (§22.16 "the partially-constructed instance unwinds under
        // §22.9's ordinary machinery"): a violated invariant here disposes
        // + frees `obj` before ContractViolationException propagates,
        // matching §22.9's ordinary cleanup shape so the heap block and
        // every owned field are reclaimed.
        emitInvariantCheck(obj, rt->declaration,
                            /*disposeAndFreeOnThrow=*/true);
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        // #1833 (b): the importedGenericWithBody path recorded
        // discardOwnedOutSlots_ entries via buildOrderedArgs at line 3999
        // for any `out _` against an `out owned` ctor parameter. The
        // inlined ctor body wrote each scratch. Dispose them here before
        // returning, mirroring lowerCall's loop shape.
        disposeNewDiscardOwnedOutSlots(newDiscardOwnedBase);
        return obj;
    }

    // fix #2860 (§2.8.2 clause 2 / §22.8 step 2 / §7.9, v0.240): `&&
    // !importedGenericNoBody`. This branch's every exit is a `return obj`, so
    // it swallowed the no-explicit-ctor construction sequence below for EVERY
    // imported type. For a NON-generic import that is right — its field
    // initializers ran in the exported `bz$<Type>$init` called at the top of
    // lowerNew (declareImportedInit). An imported GENERIC has no such symbol
    // (it is monomorphized per instantiation, §14.1), so `emitInstanceInit`
    // installed the per-instantiation vtable and NOTHING ever ran §22.8 step 2:
    // every field of `new ImportedGeneric<T>()` kept its §7.9 default, with no
    // diagnostic at any severity. §2.8.2 clause 2 is the rule broken — "An
    // external instantiation behaves identically to a same-module instantiation
    // of the same generic at the same arguments" — and the same declarations
    // compiled as ONE module initialize correctly.
    //
    // Falling through instead of duplicating the sequence here is deliberate:
    // `importedGenericNoBody` implies `!hasAnyConstructor(t)`, so `ctorForArgs`
    // below returns null and control reaches the `else if (!hasAnyConstructor)`
    // branch — the SAME code path a local no-explicit-ctor class takes, with its
    // §22.7-step-2 throwing-initializer cleanup (#1291) and its `currentThis_ =
    // obj` discipline already correct. Everything this branch would have done
    // for a ctor-less type is a no-op anyway (no ctor to call; its trailing
    // emitNewPendingCheck is re-emitted by the no-ctor branch).
    if (ctx_.isImported(t) && !importedGenericNoBody) {
        // fix #2639 (§22.3 "Parameter modifiers" / §22.4, v0.236): a BY-REF
        // argument modifier does not make an argument non-positional. This
        // predicate is the branch's only admission gate and it originally
        // rejected ANY non-empty `a.modifier`, i.e. every `ref` / `out` /
        // `out var` / `out owned var` / `out using var` argument. The branch's
        // only other exit is the `return obj` below, which emits NOTHING — so
        // `new ImportedT("x", out var n)` allocated the instance, ran
        // `bz$T$init` (vtable + field initializers) and then SILENTLY SKIPPED
        // the `bz$T$ctor` call entirely. The constructor body never ran: every
        // ctor-assigned field kept its zero/default, and the `out` recipient
        // was never written. No diagnostic (an `internal:` #2491 note appears
        // only if the recipient is later read in a position that then fails to
        // lower). Read off `BEZEL_DUMP_IR` for the issue's own repro: `main`
        // contains `call void @"bz$XLib.Widget$init"(ptr %2)` and no `$ctor`
        // call at all, while the same class with a plain `constructor(a: int)`
        // correctly emits `call void @"bz$YLib.Gadget$ctor"(ptr %2, i32 2)`.
        //
        // Everything the by-ref case needs is ALREADY implemented inside the
        // branch and was simply unreachable for a `new`: the per-argument loop
        // reads `formalMod` off the imported ConstructorSymbol and takes either
        // §22.3's alias path (`lvalueAddr`) or its bracket path
        // (`emitAccessorRefArgCopyIn` / `emitAccessorRefArgWriteBacks`, #2319),
        // and `declareImportedCtor` (LlvmBackend_Setup.cpp:2715) already types
        // a by-ref parameter as `ptrTy()`. The guard predates that work
        // (it arrived verbatim with the f291057 TU split, before #2283/#2319
        // added the by-ref handling), so the two never met.
        //
        // Deliberately NARROW: only `isByRefMod` modifiers are admitted here.
        // A NAMED argument and an `out _` DISCARD stay excluded — this loop is
        // positional and records no `discardOwnedOutSlots_` entry, so neither
        // is handled — as do the by-value ownership modifiers `owned` /
        // `scoped`, whose transfer bookkeeping (`nullTransferredOwned`) lives
        // in `buildOrderedArgs`, which this path does not use. Those shapes
        // keep today's behaviour exactly; each is the same silent-no-ctor-call
        // defect and is FILED separately rather than widened into here.
        // fix #2658 (§17.13 "The discard `_`" / §22.6 / §22.3, v0.236): the
        // `out _` DISCARD is admitted too, against a plain borrowing `out`
        // formal. fix #3081 (§22.6, v0.244) widened the admission to `out owned`
        // formals AS WELL: §22.6's "out _. The discard form applies to both
        // parameter kinds" mandates it. The plain `out` and `out owned` paths
        // share the same scratch allocation; the `out owned` half adds the
        // scratch-slot ownership bookkeeping (`discardOwnedOutSlots_` +
        // #1833(a) unwind entry) below — exactly mirroring the in-module
        // method-call path's buildOrderedArgs #1301 block (LlvmBackend_Assign.cpp
        // :10439), which already implemented both.
        //
        // The admitted set is exactly what §17.13 / §22.6 sanction and no more.
        // §17.13's out-argument position reads: "As a positional argument with
        // the `out` modifier, `_` discards the out-parameter result", and the
        // section opens "Discard is permitted in three positions; in every
        // other position `_` is treated as a normal identifier." So `ref _` is
        // NOT a spec-sanctioned discard form and is deliberately NOT admitted
        // here — an earlier draft of this fix did admit it and made it work,
        // which would have committed the backend to a form the spec does not
        // define. (The parser does set `isDiscard` for `ref _`, so the form
        // reaches codegen at all; whether it should be rejected in sema is a
        // separate question, FILED rather than settled here.) It now falls to
        // the LOUD residual below, which is the honest outcome for a shape this
        // path cannot lower.
        //
        // §17.13 also authorizes the scratch slot the loop below allocates:
        // "`out _` does not introduce a variable; the receiving slot is
        // internal to the compiler and is not observable. The argument is
        // type-inferred from the parameter's declared type."
        sema::ConstructorSymbol* cs = ctorForArgs(t, ne.args);
        auto importedFormalMod = [&](std::size_t ai) -> std::string_view {
            if (cs && ai < cs->params.size())
                if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                        cs->params[ai]))
                    return ps->modifier;
            return {};
        };
        bool allPositional = true;
        for (std::size_t ai = 0; ai < ne.args.size(); ++ai) {
            const auto& a = ne.args[ai];
            if (!a.name.empty()) { allPositional = false; break; }
            if (a.isDiscard) {
                // fix #3081 (§22.6 "out _", v0.244): the discard form applies to
                // BOTH `out T` AND `out owned T` (§22.6, v0.244: "against
                // `out T` the received value is simply dropped (§17.13);
                // against `out owned T` the discarded value is an unbound owned
                // temporary disposed at the end of the full statement
                // (§28.3)"). The plain `out` case was the #2658 admission; this
                // widens to `out owned` so the scratch-slot + disposal bookkeeping
                // below can run. Refining the type-eligibility predicate
                // (class/interface-only — the same one buildOrderedArgs' #1301
                // block uses, the same one fix #3142 imported-side widened) lives
                // at the scratch allocation site, not here, so a non-pointer
                // `out owned` (a future builtin handle, say) still admits and
                // passes through unchanged.
                std::string_view fm = importedFormalMod(ai);
                if (fm != "out" && fm != "out owned")
                    { allPositional = false; break; }
                continue;
            }
            // fix #2659 (§22.3 "Parameter modifiers" / §28.8, v0.237): the
            // by-value `owned` CALL-SITE MARKER against an `owned` formal is
            // admitted. It has to be: §22.3 makes the marker MANDATORY on a
            // named owned binding ("Omitting the marker on a named owned
            // binding is a compile error naming the fix"), so with the ownership
            // half of #2659 fixed — an imported `owned` formal is finally SEEN
            // as an owned sink — this is the ONLY spelling that reaches codegen
            // for `new ImportedT(owned r)`. Leaving it out would trade the old
            // false reject for a silently default-initialized instance plus a
            // leaked argument, which is worse.
            //
            // Lowering is identical to a plain positional argument: `owned` is
            // by VALUE, `isByRefMod("owned")` is false, so the loop below takes
            // its `lowerExpr` path and passes the same pointer a borrow would.
            // The only extra obligation is #1918's transfer bookkeeping (null
            // the caller's slot at the "call about to run" point), performed
            // explicitly at the call below since this hand-rolled path does not
            // use buildOrderedArgs.
            //
            // fix #2687 (a) / §22.3 "Parameter modifiers" / §28.14 (v0.278):
            // the by-value `scoped` CALL-SITE MARKER against a `scoped` formal
            // is admitted on the same one-line predicate. §22.3: "a class- or
            // interface-typed `scoped` argument is written `M(scoped x)` —
            // the same mark-it-at-the-site discipline as the `move` marker
            // above, though `scoped` remains an *argument modifier*." Lowering
            // is again identical to a plain positional argument — `scoped` is
            // by value, `isByRefMod("scoped")` is false, the loop below takes
            // its `lowerExpr` path, and the pointer the callee stores in a
            // non-`owned` field of the constructed instance is the same value
            // a borrow would yield. The freeze obligation is enforced
            // statically by `MethodOwnership::scopedFormalAt`'s NewExpr arm
            // (Ownership_Checks.cpp), which #3176 widened to the resolved
            // ConstructorSymbol, so the call-shape is fully checked before
            // lowering — there is no runtime freeze-call this predicate has to
            // guard. Pre-fix this combination fell through to the bare
            // `return obj` branch and tripped #2658's loudness warning, so
            // the field the ctor body would have set kept its default — a
            // program that compiled but with a degraded instance.
            if (!a.modifier.empty() && !isByRefMod(a.modifier) &&
                !(a.modifier == "owned" && importedFormalMod(ai) == "owned") &&
                !(a.modifier == "scoped" && importedFormalMod(ai) == "scoped"))
                { allPositional = false; break; }
        }
        // fix #2658: whether the `bz$T$ctor` call was actually emitted. Every
        // path out of the block below that does NOT emit it produces an object
        // whose constructor body never ran — a silent wrong answer — so the
        // residual is reported rather than dropped (see the guard after it).
        bool emittedImportedCtorCall = false;
        // fix #3082 (§22.5 / §2.8.2, v0.244): an imported constructor with
        // optional parameters is callable with fewer arguments, and the
        // omitted formals take their declared defaults. The previous
        // `ne.args.size() == cs->params.size()` predicate was a §22.5 / #2.8.2
        // violation — a caller in another module could not omit any default
        // form, while the in-module twin could. Sema now admits the call
        // (checkImportedNewExprArgs over `FormalsView::of(*cs)`), so the
        // codegen path must follow: arity-≤-count with every unfilled
        // trailing formal carrying a §22.5 default. Defaults are materialized
        // by `importedCtorDefaultArgValue` (the ctor twin of
        // `importedDefaultArgValue`) at the same point the
        // imported-method-call path does it.
        if (cs && allPositional && ne.args.size() <= cs->params.size()) {
            llvm::Function* ctorFn = declareImportedCtor(cs);
            std::vector<llvm::Value*> args;
            args.push_back(obj);
            bool ok = true;
            auto* fnTy = ctorFn->getFunctionType();
            // #1968 / §28.3: reclaim the uninitialized `obj` if an argument throws
            // during evaluation (bz$init installed only the vtable for an imported
            // type WITH a ctor — every field is zero until the imported ctor runs).
            // Restored before the imported ctor call, whose throw edge is handled
            // by emitNewPendingCheck below.
            llvm::BasicBlock* savedArgUT = unwindTarget_;
            if (!ne.args.empty()) unwindTarget_ = newArgUnwindCleanup(obj);
            // fix #2319 / §22.3: accessor-mediated `ref`/`out` argument bracket
            // (copy-IN) recorded here, flushed (copy-OUT) after the call. This
            // path iterates ne.args manually (no buildOrderedArgs), so the same
            // emitAccessorRefArgCopyIn / emitAccessorRefArgWriteBacks helpers
            // lowerCall uses for IMPORTED methods (LlvmBackend_Calls2.cpp ~6843)
            // are invoked directly. A plain local/field `ref`/`out` argument
            // still resolves through lvalueAddr (the alias path), so the
            // bracket fires only for indexer slots / properties — the §22.3
            // shapes that have no address to alias.
            std::vector<AccessorRefArg> accessorRefArgs;
            for (std::size_t ai = 0; ai < ne.args.size(); ++ai) {
                // fix #2658 (§17.13 "The discard `_`", v0.236): an `out _`
                // DISCARD has no destination lvalue — the parser leaves `value`
                // null with `isDiscard` set, so it MUST be handled before the
                // `!value` guard below (that guard is what made
                // `new ImportedT(…, out _)` abort the whole call and emit no
                // ctor at all, #2658). §17.13: "`out _` does not introduce a
                // variable; the receiving slot is internal to the compiler and
                // is not observable. The argument is type-inferred from the
                // parameter's declared type." So: allocate a throwaway scratch
                // of the FORMAL's type and pass its address.
                //
                // The type comes from the imported
                // `ParameterSymbol::resolvedDeclType` — the same field
                // `declareImportedCtor` builds the callee's own signature from,
                // so the scratch and the formal cannot disagree on width (the
                // #2639/#2629 wild-write lesson). This mirrors buildOrderedArgs'
                // #1301 block (LlvmBackend_Assign.cpp:7657), which reads the
                // ast::Param instead because its callee has a decl.
                //
                // ⚠ THE ZERO-STORE GOES IN THE ENTRY BLOCK, NOT HERE. This is
                // the placement `ensureOutVarSlots` already measured and split
                // on — see its comment at LlvmBackend_Calls2.cpp:1685-1697 and
                // the entry-block store it emits at :1702-1704. `entryAlloca`
                // (LlvmBackend_impl.hpp:2029-2033) hoists the alloca to the
                // function's entry block, so in a LOOP every iteration shares
                // ONE slot. A store emitted here — inside the loop body — would
                // re-null that slot on each iteration, wiping the pointer the
                // previous iteration left there BEFORE the callee's
                // release-of-prior on `n := …` (LlvmBackend_Calls2.cpp:1660-1668)
                // could consume it, leaking one object per iteration. That path
                // measured 5 objects / 195 B leaked on a 5-iteration loop versus
                // 1 / 39 B for the entry-block store. Nothing needs a
                // per-iteration reset here, because unlike a real `out var`
                // recipient a discard carries NO §7.9 default-value promise —
                // §17.13 says the slot is "not observable", so no program can
                // read it and no iteration can observe the leftover. Store once,
                // next to the alloca, purely so the first call never sees
                // garbage. (Pre-fix nothing was written at all — no call was
                // emitted — so a call-site store would be a NEWLY reachable
                // leak, not a pre-existing one.)
                if (ne.args[ai].isDiscard) {
                    llvm::Type* scratchTy = nullptr;
                    const sema::ParameterSymbol* ps = nullptr;
                    if (ai < cs->params.size())
                        if ((ps = dynamic_cast<sema::ParameterSymbol*>(
                                cs->params[ai])))
                            if (ps->resolvedDeclType)
                                scratchTy = lowerTypeRef(ps->resolvedDeclType);
                    if (!scratchTy) { ok = false; break; }
                    llvm::AllocaInst* scratch = entryAlloca(scratchTy);
                    llvm::IRBuilder<> init(scratch->getParent(),
                                           std::next(scratch->getIterator()));
                    init.CreateStore(
                        llvm::Constant::getNullValue(scratchTy), scratch);
                    // fix #3081 (§22.6 "out _" / §17.13 / §28.3, v0.244):
                    // `out _` against an `out owned T` formal: §22.6 mandates
                    // the discard form is "an unbound owned temporary disposed
                    // at the end of the full statement", so the produced value
                    // must reach the disposal loop at the bottom of lowerNew
                    // (`disposeNewDiscardOwnedOutSlots(newDiscardOwnedBase)`,
                    // already called at every return of this branch — the
                    // imported path's symmetry with the in-module method-call
                    // path's `discardOwnedOutSlots_` loop). Mirror the build-
                    // OrderedArgs #1301 block (`LlvmBackend_Assign.cpp:10439`)
                    // — null-initialize, push onto `discardOwnedOutSlots_`,
                    // register the #1833(a) `unwindOnly dynamicDispose` entry so
                    // a callee that writes-then-throws does not leak.
                    //
                    // The kind eligibility is the SAME class/interface gate
                    // buildOrderedArgs uses: dynamic disposal dispatches through
                    // vtable slot 2, which exists only for class/interface. A
                    // value-typed or builtin-collection/handle `out owned` is
                    // left untouched here — the slot is sized by the formal,
                    // passed, and ignored; if no value is produced on the
                    // normal-return edge the slot just stays null. Gated on
                    // pointer-typed (the vtable dispatch) AND on a Named kind
                    // with a non-null `declaration` (the #3142 fix widened this
                    // off `->decl` so imported classes pass the same check).
                    if (ps && ps->modifier == "out owned" &&
                        scratchTy == ptrTy() && ps->resolvedDeclType &&
                        ps->resolvedDeclType->kind ==
                            sema::TypeRef::Kind::Named &&
                        ps->resolvedDeclType->declaration) {
                        const ast::TypeDeclKind tk =
                            ps->resolvedDeclType->declaration->kind;
                        if (tk == ast::TypeDeclKind::Class ||
                            tk == ast::TypeDeclKind::Interface) {
                            discardOwnedOutSlots_.push_back(scratch);
                            // #1833 (a) / §22.6 / §28.3 / §17.13: the
                            // normal-return path is handled by
                            // disposeNewDiscardOwnedOutSlots below; the
                            // throw-after-assign edge needs the same
                            // null-guarded dynamic-dispose-on-unwind the
                            // in-module method-call path registers. The
                            // `unwindOnly` flag keeps the normal-return
                            // path unaffected.
                            if (!disposeStack_.empty()) {
                                Disposal d{scratch, nullptr};
                                d.unwindOnly = true;
                                d.dynamicDispose = true;
                                disposeStack_.back().push_back(d);
                            }
                        }
                    }
                    args.push_back(scratch);
                    continue;
                }
                if (!ne.args[ai].value) { ok = false; break; }
                // Determine the formal parameter's ref/out modifier from the
                // imported ConstructorSymbol (mirrors lowerCall's imported-
                // method arg path).
                std::string_view formalMod = importedFormalMod(ai);
                llvm::Value* v = nullptr;
                if (isByRefMod(formalMod) || isByRefMod(ne.args[ai].modifier)) {
                    // §22.3 alias path: a true-alias location — local, field,
                    // static, §15.1 inline-array element — has an address the
                    // callee can write through directly.
                    llvm::Type* et = nullptr;
                    v = lvalueAddr(*ne.args[ai].value, et);
                    // §22.3 bracket path: an indexer slot or property has no
                    // address to alias; emit copy-IN and record the copy-OUT
                    // for after the call. Mirrors lowerCall's own imported-
                    // method fallback (LlvmBackend_Calls2.cpp).
                    if (!v && (formalMod == "ref" || formalMod == "out")) {
                        llvm::Type* paramTy = nullptr;
                        if (ai + 1 < fnTy->getNumParams())
                            paramTy = fnTy->getParamType(ai + 1);
                        v = emitAccessorRefArgCopyIn(
                            *ne.args[ai].value, paramTy,
                            /*isOut=*/formalMod == "out",
                            accessorRefArgs);
                    }
                } else {
                    v = lowerExpr(*ne.args[ai].value);
                }
                if (!v) { ok = false; break; }
                // fix #2809 / §28.3 *Unbound owned temporaries* + §32.6
                // (v0.240): the third argument-building site that needed the
                // FFI-buffer temporary's reclaim — `new ImportedT(
                // CString.From(s))`. Like lowerCall's imported-method arm, this
                // path is hand-rolled because an imported ctor has no
                // reconstructed AST (`buildOrderedArgs` cannot run), so it
                // missed the registration the in-module ctor gets for free.
                // Measured before this: 17 B leaked per construction.
                // Same predicate, same `owned`-transfer exclusion (the callee
                // takes the obligation, §28.3 transfer — reclaiming here would
                // double-free).
                if (!isByRefMod(formalMod) &&
                    !isByRefMod(ne.args[ai].modifier) &&
                    ne.args[ai].modifier != "owned" && formalMod != "owned") {
                    bool argIsRawBytes = false;
                    if (isFfiBufferFactoryCall(*ne.args[ai].value,
                                               argIsRawBytes)) {
                        auto* tmpSlot = entryAllocaNullPtr();
                        builder_.CreateStore(v, tmpSlot);
                        registerFfiExprTemp(tmpSlot, argIsRawBytes);
                    }
                }
                // §5.1: widen an int argument to a wider numeric parameter, as
                // the in-module ctor-call path does, so the ABI matches.
                //
                // fix #4940 / §5.1 (v0.307): add the S? → T? lift-widen the
                // local buildOrderedArgs path's #4967 fix added. `coerceNumericToType`
                // alone widens a bare i32 to i64 but leaves an `{i8, i32}`
                // carrier untouched — an `int?` arg at a `long?` imported
                // formal then hit `verifyModule` ("Call parameter type does
                // not match function signature", `{i8,i32}` vs `{i8,i64}`),
                // and on the happy path the callee read the non-null bit as
                // 1 against the i32 zero, returning 4294967295 instead of null.
                // Mirror buildOrderedArgs's widenNullableArgPayload: extract
                // the i32 payload, coerce to i64, re-wrap into the expected
                // `{i8, i64}` struct. Numeric-only — leaves a `record?` arg
                // at a `record?` formal as a no-op (the local twin accepts
                // the same shape as-is).
                if (ai + 1 < fnTy->getNumParams()) {
                    llvm::Type* expectedArgTy = fnTy->getParamType(ai + 1);
                    if (auto* vSt = llvm::dyn_cast<llvm::StructType>(v->getType())) {
                        auto numeric = [](llvm::Type* t) {
                            return (t->isIntegerTy() && !t->isIntegerTy(1)) ||
                                   t->isFloatingPointTy();
                        };
                        if (auto* eSt =
                                llvm::dyn_cast<llvm::StructType>(expectedArgTy)) {
                            if (vSt != eSt &&
                                vSt->getNumElements() == 2 &&
                                vSt->getElementType(0) == nullablePrimHasValTy() &&
                                eSt->getNumElements() == 2 &&
                                eSt->getElementType(0) == nullablePrimHasValTy() &&
                                numeric(vSt->getElementType(1)) &&
                                numeric(eSt->getElementType(1))) {
                                llvm::Type* dstPay = eSt->getElementType(1);
                                llvm::Value* conv = coerceNumericToType(
                                    builder_.CreateExtractValue(
                                        v, {1u}, "imp.ctor.arg.lift.in"),
                                    dstPay,
                                    isUnsignedTargetExpr(*ne.args[ai].value));
                                if (conv && conv->getType() == dstPay) {
                                    llvm::Value* res = builder_.CreateInsertValue(
                                        llvm::UndefValue::get(eSt),
                                        builder_.CreateExtractValue(
                                            v, {0u}, "imp.ctor.arg.lift.has"),
                                        {0u});
                                    v = builder_.CreateInsertValue(
                                        res, conv, {1u}, "imp.ctor.arg.lift");
                                }
                            }
                        }
                    }
                    if (v->getType() != expectedArgTy)
                        v = coerceNumericToType(
                            v, expectedArgTy,
                            isUnsignedTargetExpr(*ne.args[ai].value));
                }
                args.push_back(v);
            }
            // fix #3082 (§22.5 / §2.8.2, v0.244): an imported constructor
            // was called with fewer arguments than its parameter list has —
            // every unfilled TRAILING formal takes its §22.5 default. The
            // call site is positional (`allPositional` is true above), so the
            // unfilled slot is exactly `ne.args.size()` + k for k in
            // [0, cs->params.size() - ne.args.size()). The default is
            // materialized by `importedCtorDefaultArgValue`, which reads
            // `ParameterSymbol::defaultText` (the same wire data the method-
            // call twin uses) and folds it through the same
            // null / `default(T)` / constant-fold arms. Sema already
            // pre-rejected unfilled formals whose default is not
            // materializable, so this returns a value for every slot or
            // reports the same §22.5/#3082 diagnostic on the way out.
            for (std::size_t ai = ne.args.size();
                 ok && ai < cs->params.size(); ++ai) {
                llvm::Value* dv = importedCtorDefaultArgValue(*cs, ai);
                if (!dv) { ok = false; break; }
                // §5.1: same coerceNumericToType the call-site-arg branch
                // applies, so an int default landing in a wider numeric
                // formal gets the right ABI (no-op for an already-typed
                // constant; mirrors the local-twin buildOrderedArgs arm).
                if (ai + 1 < fnTy->getNumParams())
                    dv = coerceNumericToType(dv, fnTy->getParamType(ai + 1),
                                             /*isUnsigned=*/false);
                args.push_back(dv);
            }
            unwindTarget_ = savedArgUT;
            if (ok && args.size() == ctorFn->arg_size()) {
                // fix #2659 / #1918 (§28.3): the "call about to run" point —
                // every argument has been evaluated and nothing can throw
                // between here and the call. Null the caller's slot for each
                // bare owned local / `owned` parameter whose ownership this
                // call transfers, so the caller's null-guarded unwindOnly
                // disposal (registerUnwindOwnedLocal / registerUnwindOwnedParam)
                // skips a value the callee's own `owned`-parameter disposal now
                // owns — no double free on the ctor's throw edge. This is the
                // same placement and the same eligibility test buildOrderedArgs
                // applies via `nullTransferredOwned`; this hand-rolled imported
                // path never calls buildOrderedArgs, so it is done here.
                for (std::size_t ai = 0; ai < ne.args.size(); ++ai) {
                    const auto& a = ne.args[ai];
                    if (a.value && !a.isDiscard &&
                        (a.modifier == "owned" ||
                         importedFormalMod(ai) == "owned"))
                        nullTransferredOwnedLocalSlot(a.value.get());
                }
                builder_.CreateCall(ctorFn, args);
                emittedImportedCtorCall = true;  // fix #2658
            }
            // fix #2319 / §22.3: flush the copy-OUT half of every accessor-
            // mediated `ref`/`out` argument. Skipped on the throw edge per
            // §22.3's "on an exception exit no write-back occurs", matching
            // lowerCall's identical post-call discipline. The `if (ok)`
            // guard above (the call was actually emitted) plus `!diverged_`
            // (the call returned normally) together match the rule.
            emitAccessorRefArgWriteBacks(accessorRefArgs,
                                         /*emit=*/ok && !diverged_);
        }
        // fix #3268 (§22.5 / §2.8.2, v0.244): the imported-ctor NAMED-argument
        // path. The all-positional path above is the historical shape #3082
        // lifted to admit omitted-trailing defaults, and remains untouched
        // (its tail of #3081 `out owned` discard handling and the #2319 §22.3
        // accessor bracket are not in scope here). The named-arg path uses
        // the §22.5 binding sema publishes on the
        // `SemaContext::setNewExprPositionalBinding` side table
        // (`checkImportedNewExprArgs` writes it in `b.paramToArg`, the same
        // side table the imported-record path `storePrimaryCtorPositionalArgs`
        // reads via #3196).
        //
        // The §22.5 binding the side table carries is the lowering-side twin
        // `importedFormalToArg(*cs, ne.args)` (LlvmBackend_Setup.cpp, the
        // ctor twin of the method version `lowerCall` already uses at
        // LlvmBackend_Calls2.cpp:8210). The helper's identity fallback is
        // never reached here: sema's `bindArgsToParams` rejects every shape
        // that would force the fallback (a `out _` discard, an unknown
        // parameter name, a double binding, an unfilled required formal),
        // so the binding is well-defined and the helper's broader-contract
        // posture buys nothing in this arm. `importedFormalToArg` differs
        // from the side table in exactly ONE way: the side table is
        // published before `importedFormalToArg` runs, so the helper has
        // the canonical binding; the side table itself is consulted by
        // codegen purely as a "sema accepted this call" signal.
        //
        // The named-arg branch is purposefully narrow: only the basic
        // shape `importedFormalToArg` admits (named and/or positional
        // arguments with no modifier and no discard). The wider modifier
        // + discard shapes the imported method arm handles are out of
        // scope here — modifier-handling on a named-position argument
        // would require the same §22.3 bracket the all-positional arm
        // already builds (and which is presently tied to the
        // `importedFormalMod(ai)`-by-ai lookup). Filing that widening is
        // local to the ctor; the headline is the §22.5 named case the
        // issue names.
        if (!allPositional && cs && !cs->params.empty()) {
            const std::vector<int>* bindingPtr =
                ctx_.newExprPositionalBinding(ne);
            if (bindingPtr && bindingPtr->size() == cs->params.size()) {
                llvm::Function* ctorFn = declareImportedCtor(cs);
                auto* fnTy = ctorFn->getFunctionType();
                // The fill-or-default map j -> call-site arg index for
                // formal j, or -1 when formal j takes its declared default.
                const std::vector<int> formalToArg =
                    importedFormalToArg(*cs, ne.args);
                std::vector<llvm::Value*> ordered(cs->params.size(), nullptr);
                // call-site arg -> formal index, so the
                // `nullTransferredOwnedLocalSlot` post-pass (which keys off
                // the FORMAL's modifier) can look up which formal a given
                // arg actually fills. -1 for an arg the helper declined to
                // bind (an unreachable shape; the helper's identity fallback
                // is defensive — sema rejects these before the side table
                // is published).
                std::vector<int> argToFormal(ne.args.size(), -1);
                for (std::size_t j = 0; j < formalToArg.size(); ++j)
                    if (formalToArg[j] >= 0 &&
                        static_cast<std::size_t>(formalToArg[j]) <
                            argToFormal.size())
                        argToFormal[static_cast<std::size_t>(
                            formalToArg[j])] = static_cast<int>(j);
                // §28.3: arguments are evaluated eagerly, left to right
                // (same §28.3 rule `lowerCall`'s imported-method arm
                // honours). Placement in `ordered` is by formal index.
                // A FAILURE here (a `lowerExpr` returning null) is
                // indistinguishable from the all-positional arm's
                // `!v` bail: mark `ok = false`, skip the call, let the
                // #2658 loudness residual below report.
                bool ok = true;
                // #1968 / §28.3: reclaim the uninitialized `obj` if an
                // argument throws during evaluation (the imported ctor
                // has not yet been entered; every field is zero until
                // the imported ctor's body runs). The ctor's own throw
                // edge is handled by emitNewPendingCheck below, so the
                // arg-time unwind target is restored before the call.
                llvm::BasicBlock* savedArgUT = unwindTarget_;
                if (!ne.args.empty()) unwindTarget_ = newArgUnwindCleanup(obj);
                for (std::size_t ai = 0; ai < ne.args.size(); ++ai) {
                    const ast::CallArgument& a = ne.args[ai];
                    const int fj = argToFormal[ai];
                    if (fj < 0 || !a.value) { ok = false; break; }
                    const std::size_t fi = static_cast<std::size_t>(fj);
                    std::string_view formalMod;
                    const sema::TypeRef* formalTy = nullptr;
                    if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                            cs->params[fi])) {
                        formalMod = ps->modifier;
                        formalTy = ps->resolvedDeclType;
                    }
                    llvm::Value* v = lowerExpr(*a.value);
                    if (!v) { ok = false; break; }
                    // fix #3133 / §7.2 / §2.5 (v0.252): an imported
                    // ctor's formal is a `sema::TypeRef*`, so the
                    // CG-nullable block buildOrderedArgs runs against
                    // `params[idx].type` does NOT fire here. Mirror the
                    // same three lines from buildOrderedArgs exactly: a
                    // `null` literal becomes the all-zero nullable
                    // struct, a bare T becomes a wrapped nullable, a
                    // value already shaped is left alone. The named-arg
                    // path does not currently exercise §22.3 ref/out
                    // modifiers, so there is no by-ref carve-out to
                    // import — `lowerExpr` is always the by-value path.
                    if (formalTy && isNullableValueTy(formalTy)) {
                        auto* expectedSt = nullableValueStructTy(formalTy);
                        if (v->getType() != expectedSt) {
                            if (v->getType()->isPointerTy() &&
                                llvm::isa<llvm::ConstantPointerNull>(v)) {
                                v = nullableValueNull(formalTy);
                            } else if (v->getType() ==
                                       expectedSt->getElementType(1)) {
                                v = wrapNullableValue(formalTy, v);
                            }
                        }
                    }
                    ordered[fi] = v;
                }
                // Materialize every unfilled formal's §22.5 default.
                for (std::size_t j = 0; j < ordered.size(); ++j) {
                    if (ordered[j]) continue;
                    llvm::Value* dv = importedCtorDefaultArgValue(*cs, j);
                    if (!dv) { ok = false; break; }
                    if (j + 1 < fnTy->getNumParams())
                        dv = coerceNumericToType(dv, fnTy->getParamType(j + 1),
                                                 /*isUnsigned=*/false);
                    ordered[j] = dv;
                }
                unwindTarget_ = savedArgUT;
                if (ok && ordered.size() == cs->params.size()) {
                    std::vector<llvm::Value*> args;
                    args.push_back(obj);
                    for (std::size_t j = 0; j < ordered.size(); ++j)
                        args.push_back(ordered[j]);
                    builder_.CreateCall(ctorFn, args);
                    emittedImportedCtorCall = true;  // fix #2658
                }
            }
        }
        // fix #2658 (§22.3/§22.4, v0.236): MAKE THE RESIDUAL LOUD. Every exit
        // from the block above that did not emit `bz$T$ctor` leaves an object
        // whose CONSTRUCTOR BODY NEVER RAN — the instance is calloc'd and
        // `bz$T$init` (vtable + field initializers) runs, then the branch falls
        // to `return obj` and emits nothing further. That was #2639's defect
        // and it is still reachable for every shape this path does not lower:
        // a NAMED argument, a by-value `owned` / `scoped` argument, and any
        // by-ref argument whose `lvalueAddr` returned null without the #2319
        // bracket applying (`out owned` is excluded from that bracket). A
        // silent no-op is the worst failure mode available here — the program
        // compiles clean and returns a wrong answer across a module boundary
        // — so report it.
        //
        // The predicate cannot fire for a legitimately ctor-less imported type:
        // `ctorForArgs` returned a constructor, so one exists and was selected
        // for THESE arguments; the only thing that failed is our ability to
        // lower the call to it. Warning (not Error) plus the `hasErrors()`
        // guard follows the established `internal:` degradation convention in
        // this backend (#2434/#2491), which reports the degradation without
        // turning a previously-accepted program into a hard reject.
        //
        // ⚠ THE PREDICATE IS "THE TYPE HAS A CONSTRUCTOR", NOT "ctorForArgs
        // RETURNED ONE". An earlier draft keyed this on `cs` and its commit
        // message claimed it covered "every exit that did not emit the call".
        // That was FALSE, and the counterexample is instructive:
        // `ctorForArgs` (LlvmBackend_impl.hpp:7767-7843) reaches an imported
        // constructor only through its two ARITY-EXACT branches, because its
        // defaults/`argsCanFill` fallback at :7836-7841 is itself gated on
        // `dynamic_cast<const ast::ConstructorDecl*>(cs->decl)` — null for
        // every imported constructor, the same #2374 null-decl bug this whole
        // family is made of. So an imported constructor with a DEFAULT
        // parameter, called with fewer arguments, yields `cs == nullptr`: no
        // call emitted AND, under the old predicate, no warning either. The
        // loudness guard contained its own instance of the bug it exists to
        // make loud. Scanning the type's members for any non-static
        // ConstructorSymbol closes that hole, and still cannot fire for a
        // genuinely ctor-less imported type (U6/U9 pin both directions).
        // The `cs == nullptr` case is a REAL residual, not merely an unwarned
        // one — the defaulted call is still not lowered. That is FILED.
        //
        // fix #3625 codegen half (§22.10 / §22.5 / §2.8.3 rule 4, v0.286):
        // suppress the #2658 warning when the importer-side step 0
        // (`storePrimaryCtorPositionalArgs` at line 6024, called for
        // has-explicit-ctor imported classes too) populated at least one
        // primary-parameter field. The warning text — "every field it assigns
        // keeps its default" — is FALSE for that case: the §22.8 step 0
        // stores ran, so the primary-parameter fields hold the call's args,
        // not calloc-zero. Without the suppress, the (2) shape of the issue's
        // repro (`new WithConv(7, "direct")` against the imported primary +
        // convenience ctor) would still print correctly but would emit the
        // misleading warning, defeating the spec rule the (sema) W3-L1 half
        // of #3625 reinstated. Computed inline: the step 0 actually stored
        // arguments iff `t->hasPositionalFields` (the early-return gate inside
        // `storePrimaryCtorPositionalArgs`) AND `!ne.args.empty()` (no args,
        // no stores — a degenerate case the no-warning branch handles), AND
        // the call bound to primary (not to a convenience ctor). The third
        // test reuses the `cs == nullptr` check from above: a null
        // `ctorForArgs` result is exactly "no explicit ctor matched the
        // call's arity", which for an imported has-primary-ctor class is
        // exactly the (2) shape (sema bound to primary via §22.5/§22.10
        // fall-through). The combination is the precise predicate.
        bool importedTypeHasCtor = false;
        if (rt && rt->declaration)
            for (sema::Symbol* m : rt->declaration->members)
                if (auto* c = dynamic_cast<sema::ConstructorSymbol*>(m))
                    if (!c->isStatic) { importedTypeHasCtor = true; break; }
        const bool importerStep0Populated =
            ctx_.isImported(t) && t->hasPositionalFields && !cs &&
            !ne.args.empty();
        if (importedTypeHasCtor && !emittedImportedCtorCall &&
            !importerStep0Populated &&
            !ctx_.diagnostics().hasErrors()) {
            ctx_.diagnostics().report(
                Severity::Warning, ne.span.begin,
                "internal: no constructor call could be lowered for this "
                "imported type, so the constructor body will not run and every "
                "field it assigns keeps its default (degraded to a "
                "default-initialized instance) — #2658");
        }
        // §22.9 / §28.3 (FINDING [4]): an imported constructor may throw. As for
        // an in-module ctor, the imported ctor (compiled by this same compiler in
        // the dependency) disposes its assigned owned fields + base subobject on
        // its own unwind path without freeing `this`; reclaim the instance block
        // here, the allocating site, exactly once on the unwind edge.
        emitNewPendingCheck(obj);  // CG20: an imported constructor may throw
        // §22.16 (v0.240, #2941): construction exit on the imported-ctor
        // path — evaluate the class invariant of the constructed type.
        //
        // fix #4592 (§22.16 / §2.8.2, v0.306): this used to read "a no-op for
        // the imported type itself, since the exporter already evaluated it;
        // but fires for any same-module-derived invariant chain". Neither
        // clause held — §22.16's construction-exit point is this `new` site,
        // which no exporter compiled, and the walk dropped the imported link
        // whether or not a local subclass sat above it. The imported type's
        // own clauses now evaluate here from the loader's re-parse.
        // Skipped on the throw edge. The
        // `disposeAndFreeOnThrow = true` flag wires the §22.9 unwind
        // shape: a violated invariant disposes + frees `obj` before
        // ContractViolationException propagates.
        emitInvariantCheck(obj, rt->declaration,
                            /*disposeAndFreeOnThrow=*/true);
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        // #1833 (b) / fix #3081 (§22.6, v0.244): the imported-ctor path
        // iterates ne.args manually (no buildOrderedArgs call), and now records
        // `discardOwnedOutSlots_` + `unwindOnly dynamicDispose` entries for any
        // `out owned` ctor formal whose discard is admitted (the #3081 widening
        // of #2658). disposeNewDiscardOwnedOutSlots runs at this point — under
        // `!diverged_`, null-guarded, dynamic vtable Dispose + reclaim — exactly
        // mirroring lowerCall's loop shape, so an `out _` against an imported
        // `out owned` formal disposes the produced value at the end of the
        // full statement.
        disposeNewDiscardOwnedOutSlots(newDiscardOwnedBase);
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
            // fix #2319 / §22.3: accessor-mediated `ref`/`out` argument bracket
            // — populated here (from the pre-pass via `prebuiltAccessorRefArgs`)
            // OR fresh from buildOrderedArgs; flushed after the ctor body/call
            // below. Mirrors lowerCall's method-call path (LlvmBackend_Calls2.cpp
            // ~6444). The prebuilt-reuse branch moves it verbatim (no re-eval);
            // the rebuild branch builds it fresh.
            std::vector<AccessorRefArg> accessorRefArgs;
            // §11.1 / #873: reuse pre-evaluated ctor args from the
            // prePopulateBasePositionalFields step above (to avoid
            // double-evaluating side-effecting arg expressions).
            if (cs == prebuiltCtorCs && it->second == prebuiltCtorFn &&
                !prebuiltCtorArgs.empty()) {
                args        = std::move(prebuiltCtorArgs);
                ctorArgExprs = std::move(prebuiltCtorExprs);
                // fix #2319: the pre-pass already recorded the §22.3 bracket
                // setup; move it through so the post-ctor flush below has the
                // copy-OUT records. The prebuilt args are reused verbatim, so
                // the bracket setup moves with them.
                accessorRefArgs = std::move(prebuiltAccessorRefArgs);
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
            // fix #3501 (§4.7 / §7.10 / §14.1, v0.283): the twin of the
            // pre-pass bracket above — the same carry, for the `new`-sites
            // whose arguments this branch (re)lowers rather than reusing the
            // prebuilt ones. See `carryCalleeUnitSubst`.
            carryCalleeUnitSubst(ctorArgSemaTy);
            args.push_back(obj);
            // #429 (ARGUMENT-PASS category): collect per-param arg expressions
            // so we can release any +1 string producer after the ctor runs.
            // #1955 / §28.3: null the caller's slot for a bare owned local/param
            // transferred via `owned x` into an `owned` ctor parameter, so the
            // caller's null-guarded unwindOnly disposal (left eligible by
            // UnwindIneligibleWalker for this shape) stays exactly-once.
            // #1968 / §28.3: reclaim the uninitialized `obj` if a ctor argument
            // throws during evaluation (obj is calloc'd + vtable-installed but the
            // ctor body has not run — every field is zero). Restored before the
            // ctor call / inline body, whose own cleanup then owns the block.
            llvm::BasicBlock* savedArgUT = unwindTarget_;
            if (!ne.args.empty()) unwindTarget_ = newArgUnwindCleanup(obj);
            // fix #2319 / §22.3: pass an accessor-ref-arg sink so buildOrderedArgs
            // emits the §22.3 copy-IN bracket for an indexer-slot / property
            // `ref`/`out` argument. Flushed below.
            const std::size_t bareTBase = bareTParamNarrowUnwraps_.size();
            builtArgs = buildOrderedArgs(cd->params, ne.args, args,
                                              &ctorArgExprs,
                                              /*nullTransferredOwned=*/true,
                                              &accessorRefArgs);
            // fix #4945 (§14.1 / §7.10, v0.307): see restoreCtorBareTParamCarriers.
            restoreCtorBareTParamCarriers(cd->params, bareTBase, args,
                                          ctorArgSemaTy);
            unwindTarget_ = savedArgUT;
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

                // fix #2305 / §28.14 / §27.5 / §28.9: same call-bounded
                // closure ARGUMENT-PASS release as lowerCall's, transplanted
                // to the ctor formals. `verifiedCallBoundedFormalAt` already
                // covers `ast::NewExpr` (Ownership_Checks.cpp), so the same
                // AST-shape guard applies one-for-one. `cd->params` is the
                // SOURCE-WRITTEN formals; `ctorArgExprs` is parallel to
                // `args[1..]` (args[0]=obj). Exclude the receiver and
                // excluded-`out` slots via the parallel size check.
                std::vector<llvm::Value*> ctorClosureArgs;
                if (!diverged_ && !needsInlineForLayout) {
                    for (std::size_t pi = 0; pi < cd->params.size(); ++pi) {
                        const ast::Param& p = cd->params[pi];
                        if (!p.modifier.empty()) continue;
                        if (!p.type ||
                            p.type->kind != ast::TypeKind::Function)
                            continue;
                        const std::size_t ai = pi + 1;
                        if (ai < args.size() && ai - 1 < ctorArgExprs.size() &&
                            ctorArgExprs[ai - 1] &&
                            isPlusOneClosureProducer(ctorArgExprs[ai - 1]))
                            ctorClosureArgs.push_back(args[ai]);
                    }
                }

                if (!typesMismatch && !needsInlineForLayout) {
                    // fix #2305: install the unwind-edge cleanup so a
                    // propagating constructor (the ctor body throws, or its
                    // argument-eval threw and the new-arg-unwind cleanup
                    // already reclaimed `obj` and routed to the enclosing
                    // boundary) drops these temporaries on the way out.
                    // Mutually exclusive with the normal-path release below.
                    llvm::BasicBlock* savedClArgUnwind = unwindTarget_;
                    if (!ctorClosureArgs.empty())
                        unwindTarget_ = closureArgUnwindCleanup(ctorClosureArgs);
                    builder_.CreateCall(it->second, args);
                    unwindTarget_ = savedClArgUnwind;
                    // §22.9 / §28.3 (FINDING [4]): the called ctor disposed its
                    // assigned owned fields + base subobject on its own unwind
                    // path (without freeing `this`); reclaim the instance block
                    // here on the throw edge.
                    emitNewPendingCheck(obj);
                    // fix #2305: normal-edge release. Only when the ctor
                    // returned (the throw edge already dropped these via the
                    // unwind cleanup above).
                    if (!diverged_) {
                        for (llvm::Value* cl : ctorClosureArgs)
                            emitClosureRelease(cl);
                    }
                    // §22.16 (v0.240, #2941): construction exit on the
                    // explicit-ctor / non-generic / no-inline path —
                    // evaluate the class invariant of the constructed type
                    // AFTER the ctor call returns normally and BEFORE the
                    // `obj` reference is exposed to the caller. Skipped on
                    // the throw edge (emitNewPendingCheck above branched
                    // to the unwind edge). The `disposeAndFreeOnThrow =
                    // true` flag wires the §22.9 unwind shape: a violated
                    // invariant disposes + frees `obj` before
                    // ContractViolationException propagates, matching the
                    // ordinary ctor-throw cleanup (which only frees, no
                    // Dispose — the invariant-throw path is the correct
                    // shape and the ctor-throw gap is a known follow-up).
                    emitInvariantCheck(obj, rt->declaration,
                                        /*disposeAndFreeOnThrow=*/true);
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
                    auto savedRefParams = refParams_;  // fix #2283
                    llvm::BasicBlock* savedUnwindTarget = unwindTarget_;
                    std::size_t savedUnwindDepth = unwindScopeDepth_;
                    currentThis_ = obj;
                    locals_.clear();
                    paramSlots_.clear();
                    // Bind constructor params to the concrete arg values.
                    // fix #2283 / §22.3: a `ref`/`out` param's arg is the
                    // caller's storage ADDRESS (alias) or the §22.3 copy-in
                    // slot (bracket) — register it in refParams_ so the inlined
                    // body dereferences through it, as bindParameters does for
                    // a compiled ctor's ref param. paramSlots_ binding left the
                    // body loading the pointer (`add ptr, i32` — malformed IR).
                    for (std::size_t pi = 0; pi < cs->params.size(); ++pi) {
                        auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                            cs->params[pi]);
                        if (ps && isByRefMod(ps->modifier)) {
                            llvm::Type* elemTy =
                                ps->declType ? lowerType(ps->declType) : i32();
                            refParams_[cs->params[pi]] = {args[pi + 1], elemTy};
                        } else {
                            // fix #5089 (§5.1 "Nullable conversions" / §14.1
                            // "Generic declarations" / §22.7 "Constructors",
                            // v0.307): the constructor-formal sibling of
                            // #5046's generic-METHOD-formal fix. `new
                            // Box<double>(a)` with `a: int?` and
                            // `constructor(x: T?)` bound `x` (T -> double at
                            // this instantiation) — `args[pi + 1]` arrives as
                            // the CALLER's own `int?` tagged `{i8, i32}`
                            // struct, not the `{i8, double}` the specialized
                            // body's field stores / reads expect, and
                            // `buildOrderedArgs`'s own per-arg nullable-wrap
                            // (fix #4967) cannot catch this: it resolves the
                            // formal under the ENCLOSING (caller) generic
                            // substitution active at that call (see the
                            // `genericSubst_ = savedSubst` bracket around it
                            // above), where a bare `T?` formal does not yet
                            // resolve to a concrete nullable value type. This
                            // loop runs AFTER `genericSubst_` is restored to
                            // THIS type's own substitution (`ctorArgSubst`,
                            // T -> double here), the same context
                            // `emitInlineGenericMethod`'s twin binding uses,
                            // so `wrapArgForNullableValueFormal`'s case
                            // (B)/(C) resolution (`nullableTypeParamConcrete`
                            // / `resolveTypeParamRef`, both reading the ACTIVE
                            // `genericSubstSemaTy_`) now sees the concrete
                            // `double?` formal and lifts the payload
                            // (`coerceNumericToType`) before re-wrapping —
                            // exactly `wrapArgForNullableValueFormal`'s own
                            // comment. `null` stays `null`; a same-type or
                            // non-numeric (record/enum) payload is left
                            // unchanged by that helper's `numeric()` guard.
                            // `srcUnsigned` is threaded from `ctorArgExprs`
                            // (parallel to `args[1..]`, see the comment where
                            // it is built above) so an unsigned source (e.g.
                            // `uint?`) zero-extends rather than sign-extends,
                            // mirroring #5046's review correction.
                            const sema::TypeRef* formal =
                                ps ? (ps->declType
                                          ? ctx_.resolvedType(*ps->declType)
                                          : ps->resolvedDeclType)
                                   : nullptr;
                            bool argUns = pi < ctorArgExprs.size() &&
                                          ctorArgExprs[pi] &&
                                          isUnsignedTargetExpr(
                                              *ctorArgExprs[pi]);
                            llvm::Value* argVal = wrapArgForNullableValueFormal(
                                args[pi + 1], formal, argUns);
                            // fix #4823 (§5.1 "Implicit numeric conversions",
                            // v0.307): `wrapArgForNullableValueFormal` only
                            // handles a NULLABLE-value formal, so a plain
                            // (non-`?`) numeric formal fell through
                            // unwidened — a non-constant `int` argument at a
                            // `double` ctor parameter bound the inlined
                            // body's alloca at the ARGUMENT's own i32 width
                            // while the body's uses of the formal (e.g.
                            // `$"F{v}"`) expect double, so the inlined body
                            // emitted a mismatched call parameter and
                            // `verifyModule` rejected the module ("Call
                            // parameter type does not match function
                            // signature!"). #4046 covered only an int
                            // LITERAL argument (constant-folded before this
                            // point). Reuse `coerceExprValueToSlot` — the
                            // same widening an ordinary compiled method-call
                            // argument gets (LlvmBackend_Layout.cpp) —
                            // rather than new logic; it is a no-op for a
                            // non-numeric / already-matching value.
                            if (formal) {
                                llvm::Type* declLTy = lowerTypeRef(formal);
                                if (declLTy && argVal->getType() != declLTy)
                                    argVal = coerceExprValueToSlot(
                                        argVal, declLTy,
                                        pi < ctorArgExprs.size()
                                            ? ctorArgExprs[pi]
                                            : nullptr,
                                        argUns);
                            }
                            auto* slot = entryAlloca(argVal->getType());
                            builder_.CreateStore(argVal, slot);
                            paramSlots_[cs->params[pi]] = slot;
                        }
                    }
                    inlineCtorBody(cs, obj);
                    unwindTarget_ = savedUnwindTarget;
                    unwindScopeDepth_ = savedUnwindDepth;
                    currentThis_ = savedThis;
                    locals_ = std::move(savedLocals);
                    paramSlots_ = std::move(savedParams);
                    refParams_ = std::move(savedRefParams);
                }
                // fix #2319 / §22.3: flush the copy-OUT half of every
                // accessor-mediated `ref`/`out` argument. Skipped on the
                // exception edge per §22.3's "on an exception exit no write-
                // back occurs", matching lowerCall's post-call discipline.
                emitAccessorRefArgWriteBacks(accessorRefArgs,
                                             /*emit=*/!diverged_);
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
            } else {
                // fix #2319: the ctor body / call never ran (buildOrderedArgs
                // failed or args size mismatched) — release the pinned
                // receiver/index values so their refcount isn't stranded.
                emitAccessorRefArgWriteBacks(accessorRefArgs, /*emit=*/false);
            }
        }
    } else if (!hasAnyConstructor(t) || directPrimaryNew) {
        // fix #3713 (§22.8 step 1+2 / §22.10, v0.286): `|| directPrimaryNew` is
        // the LOCAL half of the direct-primary gap, and it deliberately reuses
        // this branch rather than adding a second step-2 emission site. A local
        // class with a primary constructor AND an explicit one, constructed
        // through the primary (`new WithCounter(7, "direct")` against a
        // one-argument convenience ctor), reached neither arm of this cascade:
        // `ctorForArgs` found no matching explicit constructor, and
        // `hasAnyConstructor(t)` excluded it here. So `emitImplicitBase`
        // (step 1), `emitOwnFieldInit` (step 2), `emitNewPendingCheck` and the
        // §22.16 construction-exit invariant ALL failed to run, and the object
        // was returned with every non-primary field at its §7.9 default.
        // MEASURED on r12-cycle-1, single module, no import anywhere:
        // `class Derived(a: int): Base` with `Base.bx := 11`, `Derived.x := 22`
        // and `property P := 33` printed `7 / 0 / 0 / 0 / 0` where §22.8
        // requires `7 / 22 / 11 / 0 / 33`. This is the same defect the imported
        // arm above closes, so the two land together (§2.8.3 rule 4: the
        // importer must observe what a single-module compile of the same
        // declarations produces — pre-fix BOTH were wrong, identically).
        //
        // Everything this branch does is already right for the shape: step 0
        // and the base positional pre-population ran above (guarded by
        // `!boundCtorChainsThis`, which is true here because `ctorForArgs`
        // returned null — §22.8's own "a constructor that chains via `this(...)`
        // skips steps 0 and 2", and the primary is precisely the constructor
        // that does NOT skip them), and what remains is exactly steps 1+2 plus
        // the two construction-exit checks.
        //
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
        // fix #2860 (§2.8.2, v0.240): locate emitOwnFieldInit's "this imported
        // generic's field initializer did not cross the module boundary"
        // diagnostic on the `new` that would have observed the §7.9 default.
        // fix #2874: the set/restore that used to live here covered ONLY this
        // no-explicit-ctor branch, which is why the explicit-ctor construction
        // path reported the same diagnostic unlocated. It now happens once at
        // the top of `lowerNew` for every path, so nothing is needed here.
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
        // §22.16 (v0.240, #2941): construction exit (no-explicit-ctor path).
        // The implicit-base + field-init sequence is the construction body,
        // so the invariant check fires here on the normal-completion edge.
        // The `disposeAndFreeOnThrow = true` flag wires the §22.9 unwind
        // shape: a violated invariant disposes + frees `obj` before
        // ContractViolationException propagates.
        emitInvariantCheck(obj, rt->declaration,
                            /*disposeAndFreeOnThrow=*/true);
    } else if (!t->hasPositionalFields && !ctx_.diagnostics().hasErrors()) {
        // fix #4790 rework (§22.6 / §22.8, v0.307): the type declares an
        // explicit constructor and no §22.10 primary one, sema accepted this
        // `new`, and `ctorForArgs` still selected no constructor. Nothing
        // above ran — no constructor body, no field initializer — so the
        // object would leave here calloc-zero with no diagnostic at all: the
        // silent zero-fill #4790's first fix produced for boxed-interface and
        // `null`-into-`T` arguments. Codegen's selector must be at least as
        // permissive as sema's; when it is not, say so, in the established
        // `internal:` degradation convention (#2434 / #2658).
        //
        // Error, not Warning (#2924's disposition, R20 gate): the object is
        // calloc-zero, so every string / class / collection field is a null
        // pointer, and a `new` always demands its value — the program goes
        // on to read those fields as objects (#2922 measured exactly that
        // shape: a null `string` and a SIGSEGV at address 0x8). Shipping a
        // binary here would be a memory-safety hazard inside a successful
        // compile, so the compile fails instead.
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            "internal: codegen selected no constructor for this `new` that "
            "sema accepted, so no constructor body or field initializer runs "
            "and the object is default-initialized — #4790");
    }
    // CG-generic: record the substitution for this new object so that
    // lowerLocalDecl can associate it correctly even when the outer
    // savedSubst would override the lastCallGenericSubst_ from a prior call.
    if (!genericSubst_.empty())
        lastCallGenericSubst_ = genericSubst_;
    // Restore the outer generic substitution.
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    // #1833 (b): the in-module ctor paths (early buildOrderedArgs pre-pass
    // at line 3906 reused by the explicit-ctor dispatch at line 4141) may
    // have recorded `out _` discard-owned slots. Dispose them before
    // returning to the caller, exactly like lowerCall does for method
    // calls — closes the §22.4 / §28.3 top-level ctor-discard leak in
    // `var q := new R(out _);`.
    disposeNewDiscardOwnedOutSlots(newDiscardOwnedBase);
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


// §5.2 (L679, #1961): the char code-point narrowing check. Casting an integer
// to `char` requires the value to fall in U+0000–U+10FFFF excluding the
// surrogate range U+D800–U+DFFF; otherwise OverflowException. Returns the value
// as the `char` (i32) LLVM type. Shared by the explicit `char(n)` cast
// (lowerPrimCastScalar) and the §6.6 char compound-assignment/increment
// write-back (emitCompoundOp), so both enforce the identical §5.2 contract —
// the fix for #1961, where the compound/increment path skipped this check and
// stored an invalid char (a type-invariant violation).
//
// #5207: `srcUns` gives the source's signedness. A narrow unsigned source
// (byte, ushort, uint) must ZERO-extend to i64 for the range check (SExt read
// byte 200 / ushort 0xFFFF / uint as negative), and a source narrower than the
// 32-bit char must be widened, not truncated, on the way out.
llvm::Value* Emitter::emitCharRangeCheck(llvm::Value* payload, bool srcUns) {
    using P = sema::PrimitiveType;
    llvm::Type* i64ty = llvm::Type::getInt64Ty(llvm_);
    llvm::Value* v64  = srcUns ? builder_.CreateZExt(payload, i64ty, "ctoi64")
                               : builder_.CreateSExt(payload, i64ty, "ctoi64");
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
    unsigned sw = payload->getType()->getIntegerBitWidth();
    if (sw > 32) return builder_.CreateTrunc(payload, dstCharTy);
    if (sw < 32)  // value is in range (>= 0), so zext is exact for either sign
        return builder_.CreateZExt(payload, dstCharTy, "ctochar");
    return payload;
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
//   • int → char → §5.2 L679 Unicode range check + Trunc
// `srcUns` is isUnsignedPrim(srcPt), pre-computed by the caller.
llvm::Value* Emitter::lowerPrimCastScalar(sema::PrimitiveType targetPt,
                                          sema::PrimitiveType srcPt,
                                          llvm::Value* payload,
                                          bool srcUns) {
    using P = sema::PrimitiveType;
    // §5.2 (L679): casting an integer to char requires the value to be in
    // U+0000–U+10FFFF excluding the surrogate range U+D800–U+DFFF.
    if (targetPt == P::Char && payload->getType()->isIntegerTy())
        return emitCharRangeCheck(payload, srcUns);
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
                    // §5.2 (fix #4802, v0.307): uint target uses UINT32_MAX as the
                    // upper bound instead of INT32_MAX.
                    //
                    // Before #4802, every 32-bit-or-smaller target was range-checked
                    // against the signed int32 upper bound INT32_MAX — for a `uint`
                    // target this wrongly rejected every value in (INT32_MAX, UINT32_MAX],
                    // throwing away half of uint's domain.  The fix narrows the
                    // condition to `target == P::UInt` (not the broader
                    // `dstUns && dstTy == i32`): `char` shares the i32 / unsigned
                    // shape but has its OWN contract (code-point range [0, 0x10FFFF]
                    // excluding surrogates — §5.2 char semantics) which is enforced
                    // elsewhere; widening the upper bound for char too would change
                    // char's behaviour rather than fix it, and is out of scope for
                    // #4802.  The sibling ulong arm (≥32-bit) was already fixed by
                    // #4785 via a dedicated `bz_decimal_to_uint64` helper — this is
                    // the same defect shape at the 32-bit-or-smaller codegen dispatch.
                    //
                    // For the other unsigned narrower targets (ushort, byte) the
                    // existing trunc + roundtrip check (ZExt then compare back to
                    // the i32 value) already correctly bounds at USHRT_MAX /
                    // UINT8_MAX; the wider signed-range upper bound above the roundtrip
                    // is harmless (the roundtrip catches anything outside the narrower
                    // range), so we leave it unchanged.
                    llvm::Type* dstTy = lowerPrimitive(target);
                    bool dstUns = isUnsignedPrim(target);
                    int64_t hiBound = (target == P::UInt)
                        ? (int64_t)(uint64_t)UINT32_MAX
                        : (int64_t)INT32_MAX;
                    llvm::Value* lo = builder_.CreateICmpSLT(
                        v64, llvm::ConstantInt::get(i64(), (int64_t)INT32_MIN),
                        "dec.ilo");
                    llvm::Value* hi = builder_.CreateICmpSGT(
                        v64, llvm::ConstantInt::get(i64(), hiBound),
                        "dec.ihi");
                    emitGuardThrow(builder_.CreateOr(lo, hi), "OverflowException");
                    llvm::Value* v32 = builder_.CreateTrunc(v64, i32(), "dec.toi32");
                    if (dstTy == i32()) return v32;
                    // Further narrowing (e.g. to i8/i16): check sub-range and trunc.
                    llvm::Value* trunc = builder_.CreateTrunc(v32, dstTy);
                    llvm::Value* back = dstUns
                        ? builder_.CreateZExt(trunc, i32())
                        : builder_.CreateSExt(trunc, i32());
                    emitGuardThrow(builder_.CreateICmpNE(back, v32), "OverflowException");
                    return trunc;
                } else {
                    // int64 or uint64 target.
                    // #4785 / §5.2 (v0.307): ulong targets must route through
                    // bz_decimal_to_uint64 (uint64 semantics: rejects negatives
                    // and values > uint64.MaxValue) — the signed helper
                    // bz_decimal_to_int64 silently rejects the whole range
                    // (int64.MaxValue, uint64.MaxValue], which is exactly half
                    // of `ulong`'s domain.  The long-target arm is unchanged.
                    if (target == P::ULong) {
                        auto* fn = declareC("bz_decimal_to_uint64", i64(),
                                           {ptrTy(), ptrTy()});
                        llvm::Value* v64 = builder_.CreateCall(
                            fn, {srcSlot, getOVEVtable()}, "dec.tou64");
                        emitPendingCheck();
                        return v64;
                    }
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
        // #5207: shared with lowerPrimCastScalar; honours source signedness
        // and widths below 32 bits (byte/sbyte/short/ushort).
        bool srcUnsC = isUnsignedTargetExpr(argE);
        if (auto* st = ctx_.exprType(argE))
            if (st->kind == sema::TypeRef::Kind::Named && st->declaration &&
                st->declaration->kind == ast::TypeDeclKind::Enum)
                srcUnsC = enumIsUnsigned(st->declaration);
        return emitCharRangeCheck(src, srcUnsC);
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


std::vector<std::int64_t> Emitter::enumDeclaredValueSet(
        const sema::TypeSymbol* ts) {
    std::vector<std::int64_t> vals;
    if (!ts || ts->kind != ast::TypeDeclKind::Enum) return vals;
    // The value set comes from the SHARED sema/EnumValues.hpp computation, not
    // from `enumValue_`, so codegen's runtime check and sema's constant check
    // (ExprTypecheck_Calls.cpp) can never disagree about which values are
    // declared — a divergence would mean either a spurious runtime raise on a
    // constant sema accepted, or a silent pass on one it would have rejected.
    // In particular both DECLINE together (empty set here) when some constant's
    // initializer does not fold: sema's fallback would then be the §10.2
    // auto-increment default, which is a guess, and no rule may fault on it.
    sema::EnumDeclaredValues declared = sema::enumDeclaredValues(*ts);
    if (!declared.complete) return vals;
    for (const auto& c : declared.constants) vals.push_back(c.second);
    // §10.2: same-valued constants denote ONE value — the declared-value set is
    // the set of DISTINCT values. Order by the underlying type's own signedness
    // so the contiguity test below (and its unsigned range compare) is exact
    // for a `ulong`-backed enum whose values run past long.MaxValue.
    const bool uns = enumIsUnsigned(ts);
    std::sort(vals.begin(), vals.end(),
              [uns](std::int64_t a, std::int64_t b) {
                  return uns ? static_cast<std::uint64_t>(a) <
                                   static_cast<std::uint64_t>(b)
                             : a < b;
              });
    vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
    return vals;
}

llvm::Value* Emitter::enumDeclaredValueTest(const sema::TypeSymbol* ts,
                                            llvm::Value* v) {
    if (!ts || !v || !v->getType()->isIntegerTy()) return nullptr;
    std::vector<std::int64_t> vals = enumDeclaredValueSet(ts);
    if (vals.empty()) return nullptr;  // unknown value set — decline to check
    const unsigned bits = v->getType()->getIntegerBitWidth();
    // Normalize a declared value to `v`'s width, as raw bits: a `byte`-backed
    // constant 200 is the i8 pattern 0xC8, which sign-extends to -56 — comparing
    // the int64 forms directly would miss it.
    auto atWidth = [&](std::int64_t x) {
        return llvm::APInt(bits, static_cast<std::uint64_t>(x),
                           /*isSigned=*/true);
    };
    // A CONSTANT operand is answered at compile time — no runtime compare is
    // emitted for a cast of a declared constant (§5.4's constant arm has
    // already rejected an undeclared one in sema; a constant that survives to
    // here and is still undeclared folds to a `false` test, i.e. an
    // unconditional raise, rather than being waved through).
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        for (std::int64_t d : vals)
            if (ci->getValue() == atWidth(d)) return builder_.getTrue();
        return builder_.getFalse();
    }
    // A CONTIGUOUS value set (the overwhelmingly common `0, 1, 2, …` shape) is
    // one unsigned range compare: `(v - lo) u<= (hi - lo)`. The subtraction
    // wraps, so the form is exact for a negative `lo` too.
    bool contiguous = true;
    for (std::size_t i = 1; i < vals.size(); ++i)
        if (atWidth(vals[i]) != atWidth(vals[i - 1]) + 1) {
            contiguous = false;
            break;
        }
    if (contiguous) {
        llvm::Value* lo =
            llvm::ConstantInt::get(v->getType(), atWidth(vals.front()));
        llvm::Value* span = llvm::ConstantInt::get(
            v->getType(), atWidth(vals.back()) - atWidth(vals.front()));
        llvm::Value* off = builder_.CreateSub(v, lo, "enum.declared.off");
        return builder_.CreateICmpULE(off, span, "enum.declared");
    }
    // Sparse set: an OR-chain of equality compares (enums are small; a switch
    // would need extra blocks and a PHI for the same answer).
    llvm::Value* acc = nullptr;
    for (std::int64_t d : vals) {
        llvm::Value* eq = builder_.CreateICmpEQ(
            v, llvm::ConstantInt::get(v->getType(), atWidth(d)),
            "enum.declared.eq");
        acc = acc ? builder_.CreateOr(acc, eq, "enum.declared.or") : eq;
    }
    return acc;
}

llvm::Value* Emitter::tryLowerEnumFromUnderlyingOrNull(
        const ast::CallExpr& call) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || ma->member != "FromUnderlyingOrNull" || !ma->target)
        return nullptr;
    // The receiver must NAME an enum type — this is a STATIC member. Without
    // this gate a user class declaring its own
    // `static FromUnderlyingOrNull(x): SomeEnum?` would be hijacked here: the
    // member name and the `E?` result type alone do not identify the
    // compiler-provided member. (Sema's own recognition, ExprTC::
    // enumTypeNamedBy, applies exactly this gate, so the two agree.)
    const sema::TypeSymbol* named = nullptr;
    if (sema::Symbol* tsym = ctx_.exprSymbol(*ma->target)) {
        if (auto* ts = dynamic_cast<sema::TypeSymbol*>(tsym)) {
            if (ts->kind == ast::TypeDeclKind::Enum) named = ts;
        } else if (auto* nts = dynamic_cast<sema::NestedTypeSymbol*>(tsym)) {
            if (nts->type && nts->type->kind == ast::TypeDeclKind::Enum)
                named = nts->type;
        }
        // fix #4072 (§10.4 / §27.8, v0.294; family of
        // #4038/#4048/#4050/#4051/#4070): a `type` alias of the enum
        // resolves `tsym` to a `TypeAliasSymbol`, not a TypeSymbol or
        // NestedTypeSymbol — the same missing-arm shape #4050's
        // DestructuringPattern arm discovered (sema fix alone is NOT
        // enough; codegen independently re-resolves here, with its OWN
        // matching gap). Without this arm `AliasColor.FromUnderlyingOrNull
        // (value)` reached this gate with `named` still null, the
        // recognition fell through, the call's result type was empty
        // for codegen to dispatch on, and the slot silently degraded to
        // zero (#2434 backstop) — a SILENT WRONG VALUE, not a crash and
        // not a diagnosed rejection. Resolve through the SAME shared
        // `sema::resolveAliasToTypeSymbol` (TypeResolution.hpp,
        // #4050/#4051) ExprTC::enumTypeNamedBy now calls, so sema and
        // codegen agree on the alias-qualified spelling exactly like the
        // direct-name spelling.
        else if (auto* aliasSym =
                     dynamic_cast<sema::TypeAliasSymbol*>(tsym))
            if (auto* ts = sema::resolveAliasToTypeSymbol(ctx_, aliasSym))
                if (ts->kind == ast::TypeDeclKind::Enum) named = ts;
    }
    if (!named) return nullptr;
    // Sema types the whole call as `E?` (a nullable Named enum).
    const sema::TypeRef* rt = ctx_.exprType(call);
    if (!rt || !rt->nullable || rt->kind != sema::TypeRef::Kind::Named ||
        rt->declaration != named)
        return nullptr;
    if (call.args.size() != 1 || !call.args[0].value) return nullptr;
    const sema::TypeSymbol* enumTy = rt->declaration;

    llvm::Value* src = lowerExpr(*call.args[0].value);
    if (!src || !src->getType()->isIntegerTy()) return nullptr;
    // §10.4: "The parameter type is the underlying type `U` exactly" — sema has
    // already checked the argument is §5.1-convertible to `U`, so the only work
    // here is the width/signedness adjustment the enum's own representation
    // needs (§10.3 / #740), identical to lowerEnumCast's widening arm. No range
    // guard: a value that does not fit `U` cannot reach here (sema rejected the
    // narrowing), and the member is TOTAL — it never raises.
    llvm::Type* uTy = enumLlvmType(enumTy);
    bool srcUnsigned = false;
    if (auto* st = ctx_.exprType(*call.args[0].value))
        if (st->kind == sema::TypeRef::Kind::Primitive)
            srcUnsigned = isUnsignedPrim(st->primitive);
    llvm::Value* v = srcUnsigned
        ? builder_.CreateZExtOrTrunc(src, uTy, "enum.fu")
        : builder_.CreateSExtOrTrunc(src, uTy, "enum.fu");

    llvm::Value* declared = enumDeclaredValueTest(enumTy, v);
    llvm::StructType* nt = nullableValueStructTy(rt);
    llvm::Value* present = builder_.CreateInsertValue(
        llvm::UndefValue::get(nt),
        llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
    present = builder_.CreateInsertValue(
        present, reinterpretToType(v, nt->getElementType(1)), {1u});
    if (!declared) {
        // Value set unknown (an enum whose constants carry no known values) —
        // decline to decide and report presence, matching lowerEnumCast's
        // decline-to-check posture rather than manufacturing a null.
        return present;
    }
    return builder_.CreateSelect(declared, present,
                                 llvm::Constant::getNullValue(nt),
                                 "enum.fu.opt");
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
    llvm::Value* result = nullptr;
    if (dstBits < srcBits) {
        bool dstUns = enumIsUnsigned(enumTy);
        llvm::Value* trunc = builder_.CreateTrunc(src, uTy, "enum.cast.trunc");
        llvm::Value* back = dstUns
            ? builder_.CreateZExt(trunc, src->getType(), "enum.cast.back")
            : builder_.CreateSExt(trunc, src->getType(), "enum.cast.back");
        emitEnumCastGuardThrow(
            builder_.CreateICmpNE(back, src, "enum.cast.oor"), enumTy, src,
            srcUnsigned);
        result = trunc;
    } else {
        result = srcUnsigned
            ? builder_.CreateZExtOrTrunc(src, uTy, "enum.cast")
            : builder_.CreateSExtOrTrunc(src, uTy, "enum.cast");
    }
    // fix #3044 (§5.4, v0.242): the checked cast's RUNTIME arm — "When `n` is
    // not a constant, a value equal to no declared constant raises
    // OverflowException at runtime". The narrowing guard above stays and runs
    // FIRST: it compares at the SOURCE width, so a wider out-of-range value
    // (`Color(256)` on a byte-backed enum) faults there instead of aliasing a
    // declared value after truncation. Both faults are OverflowException, which
    // is exactly §5.4's "In-range-vs-out-of-range is not a user-visible
    // distinction". A constant operand folds the test away (declared → no
    // compare at all). Both sites raise through emitEnumCastGuardThrow, which
    // carries §5.4's required message "naming the enum type and the value";
    // the message is built inside the fault block, so the succeeding path pays
    // nothing for it.
    if (llvm::Value* declared = enumDeclaredValueTest(enumTy, result)) {
        const bool resUnsigned = enumIsUnsigned(enumTy);
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(declared)) {
            if (c->isZero())  // constant, undeclared → unconditional raise
                emitEnumCastGuardThrow(builder_.getTrue(), enumTy, result,
                                       resUnsigned);
            // constant, declared → nothing to emit
        } else {
            emitEnumCastGuardThrow(
                builder_.CreateNot(declared, "enum.cast.undeclared"), enumTy,
                result, resUnsigned);
        }
    }
    return result;
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

    // fix #2464 (§9.1, v0.235): `collectRecordStorage`/`t->members` order a
    // record's BODY members ahead of its POSITIONAL fields regardless of
    // source declaration order — SymbolCollection.cpp's collectTypeDecl
    // always walks td.members (the body) before appending td.positionalFields
    // — a storage detail §32.13 clause 2 leaves implementation-defined for an
    // ordinary (non-C-compatible) record: "Field reordering and packing
    // optimizations are forbidden for classified records; all other records
    // keep unspecified layout." The §9.1 auto-generated PRIMARY constructor's
    // parameters are "the positional fields, in declaration order" — never
    // the body storage — so `args[i]` must map to the i-th POSITIONAL field
    // specifically, never to `fields[i]` by raw storage index.
    //
    // The positional subset is computed inside the fast-path gate below, as
    // the TRAILING `args.size()` entries of `fields` — NOT via
    // FieldSymbol::posParam. posParam is a real `ast::Param*`, populated by
    // SymbolCollection.cpp only for a LOCALLY-declared record; ModuleLoader.cpp
    // never sets it for an IMPORTED field (there is no wire-level "is this
    // field positional" tag on MetaMember — only the whole-type
    // MetaTypeDecl::hasPositionalFields boolean). Filtering on posParam alone
    // would leave every imported record's positional set EMPTY, breaking
    // cross-module positional-record construction entirely. Instead this
    // relies on a structural invariant that holds on BOTH sides: every
    // positional field is always exported (an auto-generated PUBLIC
    // read-only property, §9.1) and — because both
    // ModuleMetadataBuilder.cpp's export loop and ModuleLoader.cpp's
    // reconstruction loop are single linear passes over `t.members` that only
    // ever SKIP non-exported entries, never reorder — the positional fields
    // stay a single CONTIGUOUS block at the tail of `fields`/`t->members`,
    // in their own declared order, on the importer's reconstruction exactly
    // as on the exporter's own compilation. The §9.1 auto-generated primary
    // constructor's arity always equals the record's TRUE positional-field
    // count (sema rejects a mismatched call before codegen ever runs), so
    // whenever this gate is reached (an implicit-primary-ctor call, so
    // `args.size()` IS that arity), the trailing `args.size()`-length slice
    // of `fields` is exactly the positional fields, in their declared order.
    // The `args.size() <= fields.size()` bound below is a defensive guard
    // against iterator underflow on a malformed call sema should already
    // have rejected before codegen — not a claim that inequality is a
    // legitimate case.
    //
    // fix #4019 (§22.8 "Field initializers run exactly once per instance
    // construction", v0.294): resolve the constructor this call will use
    // BEFORE running the preamble's field-init loop below, so the loop can
    // be skipped for a `this(...)`-chaining constructor. §22.8's sequence
    // makes step 2 (field initializers) run ONLY at the end of a
    // `this(...)` chain — "A constructor that chains via `this(...)` skips
    // steps 0 and 2" — precisely because running it at every link would
    // "run every field initializer twice under `this(...)` chaining —
    // repeating allocations and initializer side effects, assigning
    // `readonly` fields twice". `emitChainCall` (invoked below, in the
    // explicit-constructor branch, per fix #4013) already routes the chain
    // TARGET through the shared `emitConstructorBody`/`inlineCtorBody`
    // machinery, whose existing `!isThis` gate runs `emitOwnFieldInit`
    // exactly once at the bottom of the chain — the SAME once-only
    // authority the class side already relies on (CLAUDE.md §6: reuse the
    // existing authority rather than adding a second rule). Computed once,
    // here, and reused by the explicit-constructor branch below instead of
    // a second `ctorForArgs` call.
    sema::ConstructorSymbol* explicitCs = ctorForArgs(t, args);
    bool skipPreambleFieldInit = false;
    // fix #4022 (§22.8 / §12.8 / §22.7, v0.294): an IMPORTED record whose
    // explicit ctor body could not be re-parsed (parseAndAttachCtorBody's
    // `methodSignatureIsStubSafe` declined — typically because the body names
    // a sibling class that the synthetic source never declared, e.g. a
    // `this(new Sibling(args))` chain over an imported-only sibling) leaves
    // the reconstructed `importedCs->decl` nullptr. The exported library's
    // `bz$<T>$ctor/<cs>` function in ctorFns_ still does §22.8 steps 0+1+2
    // correctly — the EXPORTER had the AST and ran the same once-only
    // authority (the `!isThis` gate in emitConstructorBody) — so we just CALL
    // it from the explicit-ctor branch below. The PREAMBLE's field-init loop
    // then has nothing to do and would otherwise double-run the body-side
    // initializers that the compiled function also runs at the chain bottom,
    // so flip `skipPreambleFieldInit` to true.
    const bool explicitCsIsImportedRecordNoBody =
        explicitCs && !explicitCs->decl && ctx_.isImported(t);
    if (explicitCs) {
        // This read is AST-gated, so it answers "not chaining" for every
        // IMPORTED record: `ConstructorSymbol::decl` is nullptr across a
        // module boundary (the #3152/#3911 seam — the SAME nullptr the #3671
        // comment further down documents for an imported body member). Both
        // #4013 and #4019 are therefore fixed on the LOCAL axis only; the
        // imported twin is filed as #4022 and wants a wire-format channel for
        // the chain bit (the `dtorBodyText` precedent, cf. #3916) rather than
        // a deeper AST read.
        // SEAM-DEBT: #4022 nullptr for an imported record, so this answers
        // "not chaining" across a module boundary.
        if (auto* explicitCd = dynamic_cast<const ast::ConstructorDecl*>(
                explicitCs->decl)) {
            bool peekIsThis = false;
            if (!explicitCd->body.empty() &&
                chainCallOf(*explicitCd->body[0], peekIsThis) && peekIsThis) {
                skipPreambleFieldInit = true;
            }
        } else if (explicitCsIsImportedRecordNoBody) {
            // fix #4022 (companion of the above SEAM-DEBT): an imported
            // record ctor whose body is gone does its step 0/1/2 in the
            // compiled `bz$<T>$ctor/<cs>` function — see the comment block
            // introducing `explicitCsIsImportedRecordNoBody` above.
            skipPreambleFieldInit = true;
        }
    }

    // §9.2 body record (or a combined-form record's non-positional storage):
    // zero the whole struct, then run every body-declared field/auto-property
    // initializer. Hoisted ahead of the positional fast path below (fix
    // #2464, was previously only reached when falling through to the
    // explicit-constructor path) — a combined-form record's positional fast
    // path fills only the POSITIONAL slots, so any body storage must already
    // be zeroed/defaulted first. A positional field's `m->decl` is the owning
    // TypeDecl (never a FieldDecl/PropertyDecl), so this loop naturally skips
    // positional fields — they have no separate initializer to run.
    //
    // The zero-fill always runs (the struct's §7.9 default watermark must be
    // established regardless of which constructor arm follows); the
    // initializer loop itself is skipped when `skipPreambleFieldInit` is set
    // (fix #4019 above) — the `this(...)` chain target reached below runs it
    // instead, exactly once.
    builder_.CreateStore(llvm::ConstantAggregateZero::get(st), slot);
    if (!skipPreambleFieldInit) {
    for (const sema::Symbol* m : fields) {
        const ast::Expr* init = nullptr;
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
            init = fd->initializer.get();
        else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
            init = pd->autoInitializer.get();  // §9.2: `property X := v;`
        // fix #3671 (§9.1 / §9.2 / §22.8 / §2.8.2, v0.286): an IMPORTED body
        // member (m->decl == nullptr, so `init` above always misses) first
        // tries the SAME full parsed-AST-expr replay `emitOwnFieldInit`
        // (LlvmBackend_Setup.cpp) uses for a class's own imported field —
        // `ctx_.importedInstanceFieldInitExpr`, the load-time-reconstructed
        // Expr* ModuleLoader's `parseAndAttachFieldInit` (#2874) publishes —
        // BEFORE falling back to the narrower constant-fold-only replay
        // (#2467/#328) below. Setting `init` here, ahead of the `if (init)`
        // block, reuses that block's existing §5.1 widening / §23.1
        // nullable-value wrap verbatim instead of duplicating them — the
        // record side deliberately does NOT also reuse
        // `emitOwnFieldInit`'s class-only retain/box/§29.7-wrap logic
        // beyond that block: §9/§9.7 make a record's reference fields
        // borrows with no disposer, so none of that applies to a by-value
        // record, exactly as this loop's `if (init)` block already omits it
        // for a LOCAL record field's initializer.
        //
        // Reach today: ModuleMetadataBuilder.cpp's `isGenericMonomorphized`
        // gate (#2871) populates the wire's `fieldInitText`/parsed-expr
        // carriage only for a GENERIC record's (or generic class's)
        // instance field — a NON-generic record's non-constant field
        // initializer is not carried across the module boundary AT ALL yet
        // (its Field branch of `convertMember` never received the
        // `|| Record` widening #2525 already gave the ConstructorSymbol
        // branch's `ctorBodyText`, for the identical "a record has no
        // cross-module call to replay an initializer through" reason —
        // filed as #3723, a writer-side gap outside this file's ownership,
        // MEASURED via a real two-module build: `public record R(a: int) {
        // public b: int := Helper(); }` still reads `b == 0` after this fix
        // when `R` is non-generic). So this lookup legitimately returns
        // null for a non-generic record and falls through UNCHANGED to the
        // fold-only path below — no regression for that shape, just not yet
        // this fix's full reach.
        if (!init && m->decl == nullptr) {
            if (const ast::Expr* parsedInit =
                    ctx_.importedInstanceFieldInitExpr(m))
                init = parsedInit;
        }
        auto idxIt = fieldIndex_.find(m);
        if (idxIt == fieldIndex_.end()) continue;
        if (init) {
            llvm::Value* v = lowerExpr(*init);
            if (!v) {
                // #1386: ICE — lowerExpr returned null for a record field/
                // property default initializer; the expression kind is not
                // yet handled.
                ctx_.diagnostics().report(
                    Severity::Error, init->span.begin,
                    "internal: lowerExpr returned null for record "
                    "field/property default initializer — #1386");
                continue;
            }
            // fix #2903 (§5.1 / §7.2 / §9.2 / §23.1, v0.240): a record body
            // field / auto-property initializer reaches its slot at the SLOT's
            // width. This loop had NEITHER direction of §5.1's numeric
            // conversion — the class-side twin (emitInstanceInit) has had the
            // widening since #408 — so measured on c7404bf every one of these
            // was wrong: `wl: long := -42` read 4294967254 (the i32 bits
            // zero-filled into the i64 slot), `wq: long? := -42` and
            // `wd: double? := 3` read 0 (a bare i32 written over the tagged
            // `{i8, T}` slot), and `nn: sbyte := -1` wrote `FF FF FF` over the
            // three following `byte` fields (255 where 0 is correct). One
            // definition, shared with the class site — see the declaration
            // comment on coerceInitializerToSlot.
            //
            // fix #3944 (§9.2 / §14.1, v0.294): `recordMemberTypeResolved`,
            // not the bare `recordMemberType` — same defect class fix #3899
            // closed in patternMatches's DestructuringPattern branch, at a
            // different call site. The caller (`lowerNew`'s Record branch,
            // ~line 6416 above) installs `genericSubst_`/`genericSubstSemaTy_`
            // for THIS record's own concrete instantiation before calling
            // buildRecordValue, so the substitution is active here. With the
            // bare (unsubstituted) type, `isNullableVal` below saw a
            // non-nullable bare `T` for a field whose ACTUAL instantiated
            // type (e.g. `Box<int?>`'s `X: T`) is a nullable value — the
            // {i8 hasValue; T value} wrap was skipped, and the raw payload
            // was stored directly into a slot LLVM had already laid out
            // (under the correctly-installed substitution) as the tagged
            // struct: a silent wrong value (the payload's low byte
            // corrupting `hasValue`, the real payload staying zero).
            const sema::TypeRef* initFty = recordMemberTypeResolved(m);
            v = coerceInitializerToSlot(v, st->getElementType(idxIt->second),
                                        isUnsignedTargetExpr(*init));
            // CG-nullable: a NULLABLE VALUE field initialized with a plain
            // value (`property N: int? := 9;`, or `property C: Color? :=
            // Color.Green;` — issue #364) must wrap the bare value into the
            // {i8 hasValue; T value} struct, like the positional path below.
            // per §9.2
            if (isNullableValueTy(initFty)) {
                auto* expectedSt = nullableValueStructTy(initFty);
                if (v->getType() != expectedSt) {
                    if (v->getType()->isPointerTy() &&
                        llvm::isa<llvm::ConstantPointerNull>(v))
                        v = nullableValueNull(initFty);
                    else if (v->getType() == expectedSt->getElementType(1))
                        v = wrapNullableValue(initFty, v);
                }
            }
            builder_.CreateStore(
                v, builder_.CreateStructGEP(st, slot, idxIt->second));
            continue;
        }
        // fix #2467 (§9.2/§2.5, v0.235): `m->decl` is null for an IMPORTED
        // body storage member — no `ast::FieldDecl`/`PropertyDecl` crosses
        // the module boundary, so `init` above always misses and this
        // initializer was silently skipped entirely, leaving the storage at
        // its `ConstantAggregateZero` default regardless of the exporter's
        // declared value (#2467's own confirmed repro: a fully public
        // `record BodOnly { public property W: int := 42; }` reads `0`
        // after import). A record's construction is fully inline and
        // by-value, with NO cross-module CALL of any kind to hook a
        // class-style `bz$<Type>$init` replay into (unlike #2452's class
        // mechanism, which needs no textual carriage because the exporter's
        // own compiled function runs the real initializer) — so this falls
        // back to the SAME constant-foldable-text wire mechanism §22.12's
        // const values and the Field-side §22.8 class layout carriage
        // already use (ModuleMetadataBuilder.cpp's renderConstExpr /
        // ctx_.importedConstValue / foldImportedConstText, issue #328
        // FINDING [3]/[7] — extended to auto-properties by this same fix).
        // No new metadata surface invented (CLAUDE.md rule 4): `valueText`
        // already "rides every member" on the wire; this just consumes an
        // existing field that had no reader for a record body member before.
        // Only a CONSTANT-FOLDABLE initializer crosses this way
        // (renderConstExpr returns "" for `new Array()`, `DateTime.Now`,
        // etc., and a PRIVATE/internal member's valueText is deliberately
        // stripped by the layout-only carriage, #2468) — a non-foldable or
        // private body-member initializer remains a documented, pre-existing
        // limitation, not newly regressed by this fix.
        //
        // A LOCAL positional field also reaches here (its `m->decl` is the
        // owning TypeDecl, never a FieldDecl/PropertyDecl, so `init` misses
        // above too) — gate on `m->decl == nullptr` (true only for an
        // IMPORTED symbol; every LOCAL symbol, positional or otherwise, has
        // a non-null decl) so a local positional field takes the same
        // no-initializer-to-run `continue` it always did, without a wasted
        // (harmless, since the map lookup would find nothing, but
        // misleading) importedConstValue probe.
        if (m->decl != nullptr) continue;
        bool replayed = false;
        const std::string* text = ctx_.importedConstValue(m);
        if (text && !text->empty()) {
            const sema::TypeRef* fty = recordMemberTypeResolved(m);
            if (llvm::Value* v = foldImportedConstText(*text, fty)) {
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
                replayed = true;
            }
        }
        // fix #3671 (§2.8.2 "No restrictions beyond visibility" / §9.1 /
        // §22.8, v0.286): mirror `emitOwnFieldInit`'s own #2874/#2860
        // construction-site diagnostic — a LOUD reject instead of a silent
        // §7.9-default zero, for the case codegen can actually detect it in:
        // an imported record (generic OR, since issue 3723 widened the
        // writer's `ModuleMetadataBuilder.cpp` gate to also carry a
        // NON-generic record's instance field text, non-generic too — or a
        // generic class monomorphizing a record-typed member) whose wire
        // carried `importedInstanceFieldInitText` (so the exporter DID have
        // a non-static field initializer here) but NEITHER the parsed-expr
        // replay above NOR the constant fold just above produced a value (a
        // reconstruction parse/sema failure — e.g. the initializer
        // references a sibling that is not part of the exported public
        // surface — or a fold that returned null). Before issue 3723's
        // writer-side widening landed, a non-generic record's initializer
        // text was not carried across the wire at all, so this positive-
        // evidence guard could not false-fire on it — that shape took the
        // silent-default path below, unchanged from before this fix. It now
        // DOES fire for that shape too (the intended, correct behavior —
        // see xmod_non_generic_record_field_init_replay_3723_test.cpp T7),
        // exactly as it always did for the generic case.
        if (!replayed && !init) {
            if (const std::string* initText =
                    ctx_.importedInstanceFieldInitText(m)) {
                ctx_.diagnostics().report(
                    Severity::Error, importedFieldInitDiagLoc_,
                    "constructing imported record `" + std::string(t->name) +
                        "` cannot run the initializer of its field `" +
                        std::string(m->name) + "` (`:= " + *initText +
                        "`): a record's construction is fully inline with no "
                        "cross-module call to hook a replay into, so an "
                        "initializer crosses the module boundary only as a "
                        "reconstructed parsed expression or a constant value "
                        "folded into the interface, and neither reached this "
                        "field — either it is not a compile-time-constant "
                        "expression and this implementation could not "
                        "reconstruct it (#2874/#3671), or it references "
                        "something not visible across the module boundary. "
                        "Per \xC2\xA7""2.8.2 this construction is rejected "
                        "rather than silently leaving the field at its "
                        "\xC2\xA7""7.9 default");
            }
        }
    }
    }  // fix #4019: end of `if (!skipPreambleFieldInit)`

    // §9.1 positional record: a bare positional argument list whose count
    // matches the POSITIONAL storage members (fix #2464: never total storage
    // — a combined form's body fields/auto-properties are not primary-
    // constructor parameters, §9.1) maps each argument to its positional
    // field by position. This is the original positional-record construction
    // path, taken only when the record has no explicit constructor.
    //
    // §22.5 / #540: named arguments on a positional record are also handled
    // here by reordering the supplied args to field-declaration order before
    // the field stores, mirroring buildOrderedArgs for class constructors.
    //
    // fix #4039 (§9.1/§9.3, v0.294): the second disjunct widens this gate for
    // a COMBINED-FORM record that ALSO declares an explicit constructor of a
    // DIFFERENT arity (`hasAnyConstructor(t)` true). Per §9.3, "[t]he auto-
    // generated members are present and the body adds further members" — the
    // only listed constructor conflict is a body ctor sharing the primary's
    // exact parameter list — so a different-arity secondary constructor does
    // not retire the auto-generated primary constructor, and a call whose
    // arity matches the POSITIONAL fields must still be able to reach it.
    // Before this fix `hasAnyConstructor(t)` disabled this path unconditionally
    // (no arity check of its own), and `explicitCs` above (fix #4019's hoisted
    // `ctorForArgs(t, args)`, REUSED here rather than calling it a second
    // time, CLAUDE.md §6) correctly finds no EXPLICIT match for such a call
    // either — so BOTH of this function's construction paths were skipped and
    // it fell to the `else if (!args.empty())` failure arm at the bottom,
    // which used to return a bare `nullptr` (see that arm's #4039 comment for
    // the resulting silent Warning-then-SIGSEGV). Widening is gated STRICTLY
    // to the case that is provably sound without depending on the record's
    // own general field-completion checking (issue #4001, separately
    // tracked, not fixed here).
    //
    // fix #4058 (§9.1/§9.3/§7.9, v0.295): #4039 drew that soundness line at
    // "the record's TOTAL storage is EXACTLY its positional fields" — i.e.
    // ZERO body storage — which is a strict subset of what is actually sound.
    // The preamble loop directly above (`if (!skipPreambleFieldInit) { for
    // (const sema::Symbol* m : fields) ... }`) runs UNCONDITIONALLY before
    // this gate and already correctly initializes every LOCAL body
    // field/auto-property that carries its own initializer (its `init` is
    // `dynamic_cast<FieldDecl>(m->decl)->initializer` or
    // `dynamic_cast<PropertyDecl>(m->decl)->autoInitializer`), regardless of
    // which construction path follows — so a body member with an initializer
    // is not actually "storage this path could leave uninitialized" the way
    // #4039's comment worried about; it is already written by the time this
    // gate runs. `bodyStorageAdmitsFastPath` below mirrors that EXACT
    // predicate (CLAUDE.md §6 — feed the same fact the preamble loop already
    // computes rather than a second, drifting one) over the body-storage
    // slice of `fields` (per the fix #2464 comment above, the positional
    // fields are always the trailing `recPosFieldCount` entries of `fields`
    // on BOTH the local and imported axis — `posParam` itself is not set for
    // an imported field, so this walks the structural block instead of
    // filtering on it). A body member with NO local decl (`m->decl ==
    // nullptr`, i.e. IMPORTED) is left conservative — still declined by this
    // predicate — since whether ITS initializer crossed the module boundary
    // is a separate, already-tracked concern (#2467/#3671/#3723) this fix
    // does not fold in. Every other combined-form shape (a body
    // field/auto-property with no initializer, local or imported) still
    // falls through to that arm's Error, which remains the correct, sound
    // answer for a shape codegen cannot prove complete on its own (§9.3's
    // own "combined form" text says nothing about implicit field
    // completeness — §7.9 owns that, and #4001 is where it is being closed).
    const std::size_t recPosFieldCount =
        sema::recordPositionalFieldSymbols(t).size();
    const bool bodyStorageAdmitsFastPath = [&] {
        for (auto it = fields.begin();
             it != fields.end() -
                        static_cast<std::ptrdiff_t>(recPosFieldCount);
             ++it) {
            const sema::Symbol* m = *it;
            // SEAM-DEBT: #4058 — body-storage predicate widens #4039 to "every member has init"; `m->decl == nullptr` for imported.
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl)) {
                if (fd->initializer) continue;
            }
            // SEAM-DEBT: #4058 — sibling of the FieldDecl read above.
            if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl)) {
                if (pd->autoInitializer) continue;
            }
            // fix #4103 (§9.1/§9.3/§7.9, v0.295): an IMPORTED body member
            // (`m->decl == nullptr`, no ast::FieldDecl/PropertyDecl crosses
            // the module boundary — the #3671-documented seam #4058's
            // SEAM-DEBT block above named) is NOT necessarily
            // uninitialised. The preamble loop directly above
            // (`if (!skipPreambleFieldInit) { for (const sema::Symbol* m :
            // fields) ... }`) runs UNCONDITIONALLY before this gate and
            // already correctly services an imported body member when the
            // exporter's wire carried an initializer across the module
            // boundary — first via `importedInstanceFieldInitExpr` (the
            // #2874/#3723 widened parsed-AST-expr replay that
            // `emitOwnFieldInit` also uses), then via
            // `importedConstValue`/`foldImportedConstText` (the #2467/#328
            // const-fold replay — which #3723 widened from GENERIC to ALL
            // imported record fields). #4058 declined this shape with the
            // comment "deliberately conservative", but the preamble loop's
            // own logic already gives the same answer the local-decl arms
            // above give: "the preamble will set this slot correctly, so
            // the fast path is sound." A member for which neither replay
            // is available still falls through to the `return false` below
            // (and the terminal Error arm, unchanged) — this only widens
            // what the predicate accepts, mirroring the preamble loop's
            // own success rather than re-deriving a coarser answer from
            // `m->decl` alone (CLAUDE.md §6 — feed the same fact the
            // preamble loop already computes, not a second drifting one).
            // Per §9.3/§9.2/§7.9, accepting a member whose initializer is
            // not visible would leave the slot at its `ConstantAggregateZero`
            // default and silently read as zero — but the fold-replay path
            // (line 9637+ above) guarantees a real value lands in the slot
            // before construction completes, so admitting the fast path
            // here is sound. Without this fix, a T8-shape cross-module
            // call (T17 below, flipped from -999 to 51 by this fix) was
            // over-rejected by the terminal Error arm.
            if (m->decl == nullptr) {
                if (ctx_.importedInstanceFieldInitExpr(m)) continue;
                const std::string* text = ctx_.importedConstValue(m);
                if (text && !text->empty()) continue;
            }
            return false;  // no initializer (and no imported replay available)
        }
        return true;
    }();
    if (!args.empty() && args.size() <= fields.size() &&
        (!hasAnyConstructor(t) ||
         (args.size() == recPosFieldCount && explicitCs == nullptr &&
          bodyStorageAdmitsFastPath))) {
        // fix #2464: the positional fields are the TRAILING args.size()
        // entries of `fields` (see the comment above) — never fields[0..N).
        std::vector<const sema::Symbol*> posFields(
            fields.end() - static_cast<std::ptrdiff_t>(args.size()),
            fields.end());
        bool allPositional = true;
        for (const auto& a : args)
            if (!a.name.empty()) { allPositional = false; break; }

        // Build a field-index-ordered pointer to each call argument.
        // For positional args: fieldArg[i] = &args[i].
        // For named args: fieldArg[i] = the arg whose .name == posFields[i]->name.
        // Fall through to the body-record path if any name is unrecognised.
        std::vector<const ast::CallArgument*> fieldArg(posFields.size(), nullptr);
        bool mappingOk = true;
        if (allPositional) {
            for (std::size_t i = 0; i < posFields.size(); ++i)
                fieldArg[i] = &args[i];
        } else {
            // §22.5 / #540: map each named argument to the field with that name.
            // fix #4776 / §22.5 (v0.307): sema defers combined-form record `new`
            // resolution to codegen, so a mixed positional+named call such as
            // `new P(1, Y := 2)` reaches this path. A named arg maps to the
            // positional field of the same name. A positional arg in a mixed call
            // maps to the positional field at the same index — the same rule the
            // all-positional arm above uses.
            for (std::size_t ai = 0; ai < args.size(); ++ai) {
                const ast::CallArgument& a = args[ai];
                if (a.name.empty()) {
                    // positional arg in a mixed call — bind to the positional field
                    // at the same index, exactly as the all-positional arm does.
                    if (ai < posFields.size()) fieldArg[ai] = &a;
                    else { mappingOk = false; break; }
                } else {
                    bool found = false;
                    for (std::size_t fi = 0; fi < posFields.size(); ++fi) {
                        if (posFields[fi]->name == a.name) {
                            fieldArg[fi] = &a;
                            found = true;
                            break;
                        }
                    }
                    if (!found) { mappingOk = false; break; }
                }
            }
            // Verify every field got an argument.
            if (mappingOk) {
                for (std::size_t fi = 0; fi < posFields.size(); ++fi)
                    if (!fieldArg[fi]) { mappingOk = false; break; }
            }
        }

        if (mappingOk) {
            for (std::size_t i = 0; i < posFields.size(); ++i) {
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
                    auto fit = fieldIndex_.find(posFields[i]);
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
                // §9.1 / §14.1 (#769): resolve the field's type through the
                // active record generic substitution — see the nullable-value
                // wrap below, which is the reason this is resolved and not read
                // off the declaration.
                const sema::TypeRef* fty = recordMemberTypeResolved(posFields[i]);
                // fix #3095 (§4.7, v0.252): the RECORD POSITIONAL-ARGUMENT
                // position of the same unit rescue the local-declaration,
                // assignment, call-argument and `return` positions make. A
                // unit-typed argument lowers to no llvm::Value, so `new R(1, ())`
                // at a `U: ()?` field took the `!v` bail below and the WHOLE
                // record construction degraded (measured: `internal: … #2434`,
                // and `b.U` then read as `null` — §4.7's two values collapsed).
                // A no-op for every non-`()?` field. // per §4.7
                //
                // fix #3098 (follow-up) / §4.7 / §8.6: the `Object` SIBLING of
                // the same rescue, which the first pass omitted — leaving this
                // one position disagreeing with the other four #2915 covers.
                // §4.7: "`var o: Object := ();` is grammatically valid and
                // produces a boxed `()`", and a record field is a location like
                // any other, so `new R(1, ())` at an `O: Object` field owes the
                // box; without it the null `v` took the bail below and the WHOLE
                // record construction degraded through #2434. Ordered after the
                // `()?` arm because the two targets are disjoint (a `()?` field
                // is not an Object/interface target, so `boxForTarget` returns
                // `v` unchanged for it, and vice versa) — the order cannot
                // change any outcome, only the reading. Both are gated on `!v`,
                // so a field that DID lower a value is untouched.
                if (!v) v = unitValueForTarget(v, fty, fieldArg[i]->value.get());
                if (!v) v = boxForTarget(v, fty, fieldArg[i]->value.get());
                if (!v) return nullptr;
                // CG-nullable: a NULLABLE VALUE field (int?, bool?, …, and a
                // nullable enum — issue #364) stores the tagged struct {i8
                // hasValue; T value}, never a bare T. Wrap a plain value
                // argument (and lower a `null` literal to the all-zero null),
                // mirroring buildOrderedArgs — without this the bare T would be
                // stored into the wider struct slot, leaving the value half
                // uninitialized. per §9.1
                // (`fty` is resolved above — for `Box<int>` with a field
                // `V: T?`, the declared `T?` must resolve to `int?` so this wrap
                // fires and the value is stored as the tagged `{i8 hasValue; int
                // value}` struct; the unresolved `T?` was mis-judged, leaving the
                // field wrong.)
                // fix #5037 (§5.1 / §9.1, v0.307): the payload width/kind
                // conversion (widen, narrow, or int→float, per the NARROW
                // nullable value field comment #743 used to carry here) is
                // `wrapNullablePositionalPayload`'s job now — see its
                // declaration comment for why the previous bare
                // `CreateSExtOrTrunc` sign-extended an unsigned narrower
                // source and had no int→float arm.
                v = wrapNullablePositionalPayload(v, fty,
                                                  fieldArg[i]->value.get());
                auto idxIt = fieldIndex_.find(posFields[i]);
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

    // §9.2 body record, or a combined-form/positional record constructed
    // through an EXPLICIT constructor: storage is already zeroed and
    // body-declared initializers have already run UNLESS this constructor
    // chains via `this(...)` (fix #4019 above — in that case the chain
    // target reached via `emitChainCall` below runs them instead); here, run
    // the matching constructor with `this` bound to the struct slot. Mirrors
    // the class construction path in lowerNew but for a by-value struct.
    // `explicitCs` is the SAME resolution `skipPreambleFieldInit` above was
    // computed from (fix #4019) — reused rather than calling `ctorForArgs`
    // a second time.
    if (sema::ConstructorSymbol* cs = explicitCs) {
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) {
            // fix #4022 (§22.8 / §12.8 / §22.7, v0.294): imported record ctor
            // whose body could not be reconstructed on the importer side
            // (parseAndAttachCtorBody declined — see
            // `explicitCsIsImportedRecordNoBody` above for the gate). The
            // EXPORTER compiled `bz$<T>$ctor/<cs>` with the chain handling
            // baked in (its own `emitConstructorBody` runs the once-only
            // `!isThis` gate at the bottom of the chain), so just call it.
            // The slot has already been zeroed and (with
            // `skipPreambleFieldInit = true` above) the field-init loop did
            // not run, so the compiled function's step 0 fills the positional
            // slots and step 2 runs the body field initializers — exactly the
            // §22.8 sequence a non-imported record's local inline path runs.
            //
            // Argument lowering is the same hand-rolled shape the class-side
            // imported ctor path (line 7401+ above) uses: declare the external
            // function, lower each positional arg through `lowerExpr`, apply
            // §5.1 numeric coercion against the formal's type, fill any
            // omitted trailing formals with `importedCtorDefaultArgValue`.
            // `ctorForArgs` above already matched this `cs`, so the arity
            // agrees; the lower-level guards (modifier handling, discard,
            // named-arg, by-ref alias vs bracket) are FILES rather than
            // silently fixed here — a `new R(out x)` against an imported
            // record with the same shape that needed the class-side widening
            // is a separate scope.
            if (explicitCsIsImportedRecordNoBody) {
                llvm::Function* ctorFn = declareImportedCtor(cs);
                auto* fnTy = ctorFn->getFunctionType();
                std::vector<llvm::Value*> callArgs;
                callArgs.push_back(slot);
                for (std::size_t ai = 0;
                     ai < args.size() && ai + 1 < fnTy->getNumParams();
                     ++ai) {
                    if (!args[ai].value) return nullptr;
                    llvm::Value* v = lowerExpr(*args[ai].value);
                    if (!v) return nullptr;
                    v = coerceNumericToType(
                        v, fnTy->getParamType(ai + 1),
                        isUnsignedTargetExpr(*args[ai].value));
                    callArgs.push_back(v);
                }
                // §22.5 / #3082: omitted-trailing defaults, mirroring
                // lowerNew's imported-ctor fill loop above. The same wire
                // channel (`ParameterSymbol::defaultText`) the method-call
                // sibling uses, with the same §5.1 numeric coercion.
                for (std::size_t pi = args.size() + 1;
                     pi < fnTy->getNumParams();
                     ++pi) {
                    llvm::Value* dv =
                        importedCtorDefaultArgValue(*cs, pi - 1);
                    if (!dv) return nullptr;
                    dv = coerceNumericToType(
                        dv, fnTy->getParamType(pi), false);
                    callArgs.push_back(dv);
                }
                builder_.CreateCall(ctorFn, callArgs);
                // §22.8 step 4 / §28: an imported ctor may throw. The
                // record value lives in the entry-block slot, not a heap
                // block, so there is nothing to reclaim here — only the
                // exception slot to surface, exactly as the local inline
                // path's `emitPendingCheck()` does above.
                emitPendingCheck();
                return builder_.CreateLoad(st, slot);
            }
            return nullptr;
        }
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
        // #1955 / §28.3: null the caller's slot for a bare owned local/param
        // transferred via `owned x` into an `owned` ctor parameter (mirrors the
        // class-ctor paths above; harmless for the common record ctor whose
        // params are value-typed — no bare owned local/param slot is nulled).
        // fix #2319 / §22.3: pass an accessor-ref-arg sink so buildOrderedArgs
        // emits the §22.3 copy-IN bracket for an indexer-slot / property
        // `ref`/`out` argument. Flushed below after lowerScopedBlock (the
        // record's inlined ctor body) returns NORMALLY. Mirrors lowerCall's
        // method-call path; helpers themselves unchanged.
        std::vector<AccessorRefArg> accessorRefArgs;
        const std::size_t bareTBase = bareTParamNarrowUnwraps_.size();
        bool builtRecArgs = buildOrderedArgs(cd->params, args, ordered, nullptr,
                                             /*nullTransferredOwned=*/true,
                                             &accessorRefArgs);
        // fix #4945 (§14.1 / §7.10, v0.307): see restoreCtorBareTParamCarriers.
        restoreCtorBareTParamCarriers(cd->params, bareTBase, ordered, recSemaTy);
        genericSubst_ = std::move(recSubst);
        genericSubstSemaTy_ = std::move(recSemaTy);
        if (!builtRecArgs) {
            // fix #2319: the ctor body never runs — release the pinned
            // receiver/index values so their refcount isn't stranded.
            emitAccessorRefArgWriteBacks(accessorRefArgs, /*emit=*/false);
            return nullptr;
        }
        llvm::Value* savedThis = currentThis_;
        auto savedLocals = locals_;
        auto savedParams = paramSlots_;
        auto savedRefParams = refParams_;  // fix #2283
        // fix #4056 / §9.4 / CG29 / §22.3 (v0.294): a record ctor body inlined
        // here lowers `this`-using expressions (e.g. `R.Touch(this)`) against
        // `currentThis_` = the by-value receiver pointer (CG29). `currentReceiverRecordType`
        // needs the parent record TypeSymbol so `coerceThisRecordValue` can load
        // the struct out at the call argument site — the methodFns_ lookup misses
        // (no MethodSymbol for a ctor body) and `currentSpecMethodSymbol_` is null
        // here. The sibling record-ctor paths (emitConstructorBody / inlineCtorBody)
        // already install the same fallback; this is its twin for the buildRecordValue
        // explicit-ctor inline path that lowerNew reaches for `new R(...)`.
        const sema::TypeSymbol* savedCtorParentRecord = currentCtorParentRecord_;
        currentCtorParentRecord_ = t;
        currentThis_ = slot;
        locals_.clear();
        paramSlots_.clear();
        // fix #2283 / §22.3: a `ref`/`out` param's arg is the caller's storage
        // ADDRESS (alias path) or the §22.3 copy-in slot (bracket path) —
        // register it in refParams_ so the inlined record ctor body
        // dereferences through it, as bindParameters does for a compiled ref
        // param. paramSlots_ binding left the body loading the pointer itself
        // (`add ptr, i32` — malformed IR), the same gap as the class-ctor
        // inline paths above.
        for (std::size_t pi = 0; pi < cs->params.size(); ++pi) {
            llvm::Value* av = ordered[pi + 1];
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(cs->params[pi]);
            if (ps && isByRefMod(ps->modifier)) {
                llvm::Type* elemTy =
                    ps->declType ? lowerType(ps->declType) : i32();
                refParams_[cs->params[pi]] = {av, elemTy};
            } else {
                auto* pslot = entryAlloca(av->getType());
                builder_.CreateStore(av, pslot);
                paramSlots_[cs->params[pi]] = pslot;
            }
        }
        // fix #2712 / §28.1 / §28.9 / §9 (v0.240): the INLINED RECORD-CONSTRUCTOR
        // member of the #2696 family (recorded on #2707) — the identical shape to
        // `inlineCtorBody`: this body is lowered into the ENCLOSING function with
        // `currentFn_` untouched, so the drop-on-overwrite release fires on the
        // constructor's own `string` parameter with no compensating +1 and frees
        // the CALLER's string. Reproduced mechanically (`new Rec($"v-{i}")` where
        // the record ctor does `s := s + "-x"`, and the caller reads the string
        // afterwards): `heap-use-after-free`, frames `bz_string_release <- main`.
        // Folded in here rather than filed because it is literally this one call
        // and the alternative was leaving a proven use-after-free in the tree.
        retainAssignedStringParams(cd->body, cs->params);
        // fix #4013 (§12.8 / §22.7 / §22.8, v0.294): a leading `this(...)` chain
        // call (§12.8 puts it in the body's first statement) was never
        // recognised here — unlike every OTHER constructor-body lowering path
        // (`emitConstructorBody`'s compiled ctor functions and the class-side
        // `inlineCtorBody`), which all route through the shared
        // `chainCallOf`/`emitChainCall` pair (Node.hpp's `chainCallOfStmt`
        // doc comment). This inline record-construction path instead handed
        // the WHOLE `cd->body` — chain-call statement included — to
        // `lowerScopedBlock`. `this(...)`'s callee is the bare identifier
        // "this" (chainCallOfStmt), which names no ordinary callable symbol,
        // so the general call-lowering machinery silently produced no IR for
        // it (the #3777-class "no diagnostic at any severity" failure mode) —
        // the chained-to constructor's body never ran. Measured (issue 4013):
        // `record R { property N: int; constructor(n:int) N:=n; constructor()
        // this(42); }` then `new R().N` read back `0`, not `42`; a second
        // repro where the chained-to ctor was the only assigner of a
        // non-nullable reference-typed field SIGSEGVed on first use, because
        // sema's DefiniteAssignment.cpp (`runRecordConstructor`,
        // `getChainingCall`) correctly treats a `this(...)`-chaining record
        // ctor as having discharged every §7.9-strict field THROUGH the
        // chain — exactly mirroring the class side's `runConstructor` — so no
        // diagnostic caught the never-executed chain at compile time either.
        // `emitChainCall` already calls the chain target's OWN compiled
        // `bz$<Type>$ctor`/`.N` function (found in `ctorFns_`, unconditionally
        // emitted for every explicit constructor including a record's, per
        // `emitConstructorBody`'s use above) against `currentThis_` — set to
        // `slot` just above — which is exactly the by-value receiver a record
        // constructor needs; a record has no base class (`emitImplicitBase`
        // no-ops via `baseClassOf` returning null), so only the `this(...)`
        // arm is reachable here, never `base(...)`.
        //
        // fix #4019: this re-derives the SAME `this(...)` fact
        // `skipPreambleFieldInit` above already computed from `explicitCs`
        // (this `cs`) — kept as a separate, local check rather than a
        // parameter threaded down from there, since this one also needs
        // `chain`/`bodyStartIdx` for the emission below, which the preamble
        // peek has no use for.
        bool chainedThis = false;
        std::size_t bodyStartIdx = 0;
        if (!cd->body.empty()) {
            if (const ast::CallExpr* chain =
                    chainCallOf(*cd->body[0], chainedThis)) {
                emitChainCall(chain, chainedThis, cs);
                bodyStartIdx = 1;
            }
        }
        lowerScopedBlock(cd->body, bodyStartIdx);
        currentThis_ = savedThis;
        locals_ = std::move(savedLocals);
        paramSlots_ = std::move(savedParams);
        refParams_ = std::move(savedRefParams);
        // fix #4056: restore the enclosing body's ctor-parent slot so a sibling
        // record construction in the same scope sees the caller's value, not the
        // just-finished ctor's parent.
        currentCtorParentRecord_ = savedCtorParentRecord;
        // fix #2319 / §22.3: flush the copy-OUT half of every accessor-
        // mediated `ref`/`out` argument. Skipped on the throw edge per
        // §22.3's "on an exception exit no write-back occurs", matching
        // lowerCall's post-call discipline. emitPendingCheck below handles
        // the thrown path.
        emitAccessorRefArgWriteBacks(accessorRefArgs,
                                     /*emit=*/!diverged_);
        emitPendingCheck();  // §28: a constructor may throw
    } else if (!args.empty()) {
        // fix #4039 (§9.1/§9.3, v0.294): arguments were supplied but no
        // constructor accepts them. Before reporting anything, narrow to
        // the SPECIFIC #4039 shape — `recPosFieldCount > 0 && args.size()
        // == recPosFieldCount` — rather than every call that reaches this
        // arm. This function is reached by every record, including a
        // BODY-ONLY record with zero positional fields whose sole
        // construction route is an explicit constructor; for that shape,
        // reaching here means `ctorForArgs` (== `explicitCs` above) failed
        // to match an EXPLICIT constructor for a reason unrelated to
        // #4039 (e.g. `ctorForArgs`'s `typeClassOf` — LlvmBackend_impl.hpp
        // — classes `int` and `decimal` differently, so an int-literal
        // argument against a `decimal`-typed explicit parameter is
        // rejected as a type mismatch even though it is a legal §5.1
        // constant-narrowing conversion — MEASURED via
        // tests/lit/sema/sema_record_equality__validating_ctor_eq_accepted_v0182_1572.bzl's
        // `record Rectangle { …; constructor(width: decimal, height:
        // decimal) … }` called as `new Rectangle(3, 4)`). That is a real,
        // separate, pre-existing silent-degrade defect (filed as #4046,
        // not fixed here — out of scope for #4039, which is specifically
        // about a POSITIONAL-field record's OWN auto-generated constructor
        // being blocked by an unrelated-arity sibling), and promoting ITS
        // silent Warning to an Error here would be exactly the false
        // widening #2434's own comment warns against — MEASURED to
        // regress that lit test's `bezelc -c` from exit 0 to exit 1 when
        // this narrowing was not yet in place.
        //
        // For the narrowed #4039 shape itself: a COMBINED-FORM record
        // (positional fields + a body) that ALSO declares an explicit
        // constructor of a DIFFERENT arity — the §9.1 positional fast path
        // above blanket-disables itself the moment `hasAnyConstructor(t)`
        // is true, with no arity check of its own, so a call whose arity
        // matches the POSITIONAL fields (not the explicit ctor) falls
        // through both this function's paths and lands here with neither
        // ever having run. Per §9.3, "[t]he auto-generated members are
        // present and the body adds further members" — the only listed
        // constructor conflict is a body ctor sharing the primary's exact
        // parameter list — so a different-arity secondary constructor does
        // not retire the auto-generated primary constructor, and
        // `new Combo(5)` against `record Combo(X: int) { …; constructor(x:
        // int, w: Widget) … }` is legal source with no reachable
        // construction path in this implementation, not a call this
        // function should silently drop.
        //
        // Before this fix, returning bare `nullptr` here made the caller's
        // `lowerLocalDecl` #2434 fallback (LlvmBackend_Setup.cpp) — which
        // exists to tolerate SIX unrelated, genuinely-permissive-typing
        // "no lowered value" shapes (§3/#605, `where T: new()`) — the ONLY
        // place this failure surfaced: a `Severity::Warning` naming neither
        // this record nor this constructor, at a clean COMPILE EXIT 0,
        // followed by a runtime SIGSEGV on the resulting zero-filled value.
        // A Warning is right for those six permissive shapes (an Error
        // there is a measured false-reject, see #2434's own comment) but
        // wrong for this one: "no constructor of this record accepts this
        // call" is never legitimate FOR THIS NARROWED SHAPE, so report it
        // LOUDLY and LOCATED here, at the exact point the front end should
        // have rejected it and did not — `importedFieldInitDiagLoc_` is
        // `lowerNew`'s `ne.span` for the whole of this function's call, set
        // once by the `ImportedFieldInitDiagLocScope` guard (fix #2874)
        // that already covers every construction path `buildRecordValue`
        // reaches, so no new location threading is added here.
        //
        // This is the CODEGEN half of a two-part defect; the SEMA half —
        // `new RecordType(args)`'s overload resolution never considering
        // the record's own explicit constructors, which both over-rejects
        // a call meant for one (#4017) and, as here, under-detects a call
        // that coincidentally matches the primary's own arity — is tracked
        // separately (#4017) and not fixed by this commit.
        if (recPosFieldCount > 0 && args.size() == recPosFieldCount &&
            !ctx_.diagnostics().hasErrors()) {
            ctx_.diagnostics().report(
                Severity::Error, importedFieldInitDiagLoc_,
                "`new " + std::string(t->name) + "` supplies " +
                    std::to_string(args.size()) +
                    " argument(s), but no constructor of `" +
                    std::string(t->name) +
                    "` accepts them: neither an explicit constructor "
                    "(\xC2\xA7""9.3) nor the record's auto-generated "
                    "positional constructor (\xC2\xA7""9.1, which takes "
                    "exactly " + std::to_string(recPosFieldCount) +
                    " argument(s), the record's positional-field count) "
                    "matches this call");
        }
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
    sema::TypeRef choiceScrutStorage;  // backs a recovered `this`-choice scrutTy
    // K8 full-expression boundary (#1795): mirror the CaseStmt scratch
    // variables for the expression-position form. The ownership pass set
    // ce->disposeOwnedScrutinee when the scrutinee is a fresh owned producer;
    // we capture scrut in an entry alloca, register the slot for unwind
    // disposal, and emit a normal-path disposal at endBB top — the same
    // dispose-exactly-once pair as the CaseStmt arm.
    llvm::AllocaInst* scrutOwnedSlot = nullptr;
    bool scrutOwnIsOwningCollection = false;
    // fix #4625 / §28.3 *`case` scrutinees extend the boundary* (v0.307): the
    // owning-CHOICE spelling of the same K8.3 boundary, which §28.3 applies "in
    // statement position exactly as in expression position". `lowerCaseStmt`
    // captured such a scrutinee (#4552/#4609/#4617); this function did not
    // capture it at ALL — no slot, no dispose-stack entry, no
    // construct-completion reclaim — so an owning match in expression position
    // leaked its payloads outright. MEASURED on
    // `var r := (case Outer.W(new Widget("a"), new Widget("b")) of
    //            W(_, _) => 1, X => 2);` over
    // `choice Outer W(owned a: Widget, owned b: Widget), X;`: neither
    // destructor ran, `LeakSanitizer: 48 byte(s) leaked in 2 allocation(s)`.
    llvm::AllocaInst* scrutChoiceSlot = nullptr;
    const sema::TypeRef* scrutChoiceRef = nullptr;
    if (ce.scrutinee) {
        scrut = lowerExpr(*ce.scrutinee);
        if (!scrut) return nullptr;
        enumTy = enumTypeOf(*ce.scrutinee);
        isStr = isStringExprType(*ce.scrutinee);
        // §7 / issue #447: pass the scrutinee's resolved TypeRef so
        // patternMatches can detect a nullable VALUE type and apply
        // hasValue-based matching instead of the pointer-based paths.
        scrutTy = ctx_.exprType(*ce.scrutinee);
        // §35.5 (v0.200, #2047): recover a choice scrutinee whose sema exprType
        // is absent — notably `this` inside a choice instance method, left
        // untyped by sema like a record `this`. choiceTypeOf recovers the
        // receiver choice (via the current choice method); when it fires, load
        // the struct value if the scrutinee lowered to a POINTER (a choice `this`
        // is passed by pointer, like a record) so patternMatches' tag
        // ExtractValue runs on the value, and synthesize the scrutinee TypeRef
        // the §35.5 choice-pattern branch keys on.
        if (!scrutTy || scrutTy->kind != sema::TypeRef::Kind::Named ||
            !scrutTy->declaration ||
            scrutTy->declaration->kind != ast::TypeDeclKind::Choice) {
            if (const sema::TypeSymbol* chTy = choiceTypeOf(*ce.scrutinee)) {
                if (scrut->getType()->isPointerTy())
                    scrut = builder_.CreateLoad(choiceStruct(chTy), scrut,
                                                "choice.this.val");
                choiceScrutStorage.kind = sema::TypeRef::Kind::Named;
                choiceScrutStorage.declaration =
                    const_cast<sema::TypeSymbol*>(chTy);
                scrutTy = &choiceScrutStorage;
            }
        }
        if (ce.disposeOwnedScrutinee && scrutTy &&
            scrutTy->kind == sema::TypeRef::Kind::Named &&
            scrut->getType()->isPointerTy()) {
            scrutOwnedSlot = entryAllocaNullPtr();
            builder_.CreateStore(scrut, scrutOwnedSlot);
            scrutOwnIsOwningCollection = common::isOwningCollectionHead(
                scrutTy->headName());
            Disposal d{scrutOwnedSlot, nullptr};
            d.unwindOnly = true;   // arm-body throw disposes the temp
            if (!scrutOwnIsOwningCollection)
                d.dynamicDispose = true;
            else
                d.collection = true;
            if (!disposeStack_.empty())
                disposeStack_.back().push_back(d);
        }
        // fix #4625 / §35.5 *The owning match* + §28.3 (v0.307): capture the
        // owning-CHOICE scrutinee, byte-for-byte the capture `lowerCaseStmt`
        // performs. An owning choice is a VALUE type, so `scrut` is a struct in
        // a register and the `scrutOwnedSlot` gate above (which requires a
        // pointer) declines it — in statement position #4552 added this arm;
        // here it was simply absent, and §28.3 requires the K8.3 boundary "in
        // statement position exactly as in expression position".
        //
        // Both of §35.5's announcing scrutinee forms are admitted, exactly as
        // in statement position: an unbound owned temporary
        // (`unboundOwningChoiceTemp`) and a named owned binding under the
        // explicit transfer marker (`case move o of …`,
        // `markedOwningChoiceScrutineeSlot`), the latter additionally CONSUMING
        // its source — "the consumed binding is left moved-from when the
        // construct completes". The Disposal entry is NOT `unwindOnly`, for
        // #4609's reason: a `throw` out of an arm body takes the unwind edge,
        // but the construct can also be left by an exception raised in a LATER
        // operand of the enclosing full expression, and `emitDisposals` skips an
        // `unwindOnly` entry when `dropUndischarged` is false.
        llvm::AllocaInst* markedSrcSlot =
            markedOwningChoiceScrutineeSlot(ce.scrutinee.get(), scrutTy);
        if (!scrutOwnedSlot && scrut &&
            (unboundOwningChoiceTemp(ce.scrutinee.get(), scrutTy) ||
             markedSrcSlot) &&
            !scrut->getType()->isPointerTy() && !disposeStack_.empty()) {
            scrutChoiceSlot = entryAlloca(scrut->getType());
            builder_.CreateStore(scrut, scrutChoiceSlot);
            scrutChoiceRef = scrutTy;
            Disposal d{scrutChoiceSlot, nullptr};
            d.unwindOnly = false;
            d.owningChoice = true;
            d.owningChoiceTy = scrutTy->declaration;
            d.owningChoiceRef = scrutTy;
            disposeStack_.back().push_back(d);
            if (markedSrcSlot)
                builder_.CreateStore(
                    llvm::Constant::getNullValue(
                        markedSrcSlot->getAllocatedType()),
                    markedSrcSlot);
        }
    }
    const sema::TypeRef* resultSemaTy = ctx_.exprType(ce);
    llvm::Type* resultTy = i32();
    if (resultSemaTy) resultTy = lowerTypeRef(resultSemaTy);

    // fix #4625 / §35.5 + §28.3 (v0.307): the captured owning-choice
    // scrutinee's NORMAL-edge reclaim at the construct's completion — K8.3's
    // boundary, the exact counterpart of the one `lowerCaseStmt` emits at the
    // top of its `casestmt.end`. Every payload the selected arm did NOT
    // transfer (the per-slot marks above decide which) is disposed here, after
    // every arm body has run, so §19.3's pattern bindings stay alive through
    // the arm body. The zero-store after it makes the reclaim exactly-once at
    // RUNTIME too: the dispose-stack entry covering a `throw` out of an arm, or
    // out of a later operand of the enclosing full expression, then walks a
    // zeroed choice (tag 0, null payload pointers) and disposes nothing. The
    // compile-time erasure alone is edge-order-dependent — #2816's lesson.
    auto emitOwningChoiceScrutReclaim = [&]() {
        if (!scrutChoiceSlot || !scrutChoiceRef) return;
        emitOwningChoiceLocationDisposal(scrutChoiceSlot, scrutChoiceRef);
        builder_.CreateStore(
            llvm::Constant::getNullValue(scrutChoiceSlot->getAllocatedType()),
            scrutChoiceSlot);
        if (!disposeStack_.empty()) {
            auto& scope = disposeStack_.back();
            for (auto it = scope.begin(); it != scope.end(); ++it) {
                if (it->slot == scrutChoiceSlot) {
                    scope.erase(it);
                    break;
                }
            }
        }
    };

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
                if (!disposeStack_.empty())
                    recordDeclScopeDepth(bp);  // fix #4523
                locals_[bp] = slot;
            // fix #2915: Kind::Unit is excluded EXPLICITLY rather than by
            // isBoxableValueType (which now admits it — §4.7/§8.6 — so a boxed
            // `()` has a runtime identity). This arm allocas and loads
            // `lowerTypeRef(btyRef)`, and the unit type lowers to LLVM `void`:
            // neither an `alloca void` nor a `load void` is constructible, and
            // this arm has no `void`→`i8` placeholder substitution of the kind
            // lowerLocalDecl applies (#1619). A `var u: ()` typed binding
            // therefore keeps EXACTLY its pre-fix lowering.
            } else if (btyRef && isBoxableValueType(btyRef) &&
                       btyRef->kind != sema::TypeRef::Kind::Unit &&
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
                if (!disposeStack_.empty())
                    recordDeclScopeDepth(bp);  // fix #4523
                locals_[bp] = slot;
            } else {
                llvm::AllocaInst* slot = entryAlloca(scrut->getType());
                builder_.CreateStore(scrut, slot);
                if (!disposeStack_.empty())
                    recordDeclScopeDepth(bp);  // fix #4523
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
        // fix #4625 / §35.5 (v0.307): the PER-SLOT discharge of the
        // construct-completion reclaim, the same call `lowerCaseStmt` makes at
        // the top of a selected arm's body (#4609's per-arm gate, narrowed to
        // per-slot by #4617). `bodyBB` is reached only on selection — a
        // guard-declined arm branches to `nextBB` and "transfers nothing"
        // (§35.5 *Guards*) — so this is exactly §35.5's boundary. The arm's
        // bindings were filled from the scrutinee VALUE, never re-read from
        // this slot, so the arm body below cannot observe the marks.
        //
        // THIS IS NOT OPTIONAL PRECISION IN EXPRESSION POSITION — it is what
        // makes the reclaim below safe. §28.3's boundary "is what keeps
        // §19.3's pattern bindings — borrows whose source is the scrutinee —
        // alive through the arm body", and in expression position such a borrow
        // can be the ARM'S RESULT and outlive the construct. MEASURED, admitted
        // by the checker today:
        //
        //   var got := (case move o of W(var w, _) => w, X => other);
        //   Console.WriteLine($"got={got.Ping()}");
        //
        // `got` is a borrow of payload `a`. Reclaiming the whole value at
        // completion — a reclaim with no discharge — disposes `a` before that
        // read: a use-after-free. Marking the slot the arm bound leaves `a` out
        // of the reclaim, which is §35.5's own answer ("transfers the case's
        // `owned` … payloads into its bindings") and #4617's sound direction:
        // failing to mark a slot the arm took is a double free, marking one it
        // did not is a leak.
        if (scrutChoiceSlot && patternBindsAnySlot(arm.pattern.get()))
            dischargeOwningMatchTransfers(scrutChoiceSlot, scrutChoiceRef,
                                          arm.pattern.get());
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
            // fix #4469 (§19.7, v0.306): a void/unit-typed arm body
            // (`(case … of Inner.IOk(var c) => Console.WriteLine(...), …)`
            // where every arm is a void-returning call) yields `v == nullptr`
            // from lowerExpr — the void-result normalization at
            // LlvmBackend_Exprs.cpp:90 collapses the call's `void` typed
            // result to a sentinel null. Pre-fix the `if (!v) return nullptr`
            // here bailed out of lowerCaseExpr BEFORE the deferred
            // `CreateBr(endBB)` loop fired, leaving the inner `case.end`
            // basic block empty (no terminator) AND the second-and-later
            // arms never processed. The outer `lowerCaseStmt`'s
            // `if (!armTerm) builder_.CreateBr(endBB)` then branched FROM
            // wherever the void call's lowering had left the insert point
            // (the cat.* machinery's last block, e.g. cat.done70) TO the
            // outer `casestmt.end` — which is well-formed for the outer
            // stmt, but the inner case.expression's `case.end` was left
            // dangling and the IR verifier rejected it. Mirror
            // lowerConditional's void handling (§17): let the arm be
            // recorded in `incomings` with a void placeholder so the
            // deferred branch fires, and skip PHI construction below
            // (`void` is not a first-class type and cannot form a PHI).
            if (!v) {
                if (resultTy && resultTy->isVoidTy()) {
                    // void result: record the arm's convergence block so
                    // the deferred branch fires; v itself stays null.
                    incomings.push_back(
                        {nullptr, builder_.GetInsertBlock()});
                    diverged_ = false;
                    builder_.SetInsertPoint(nextBB);
                    continue;
                }
                // fix #4469 (§19.7, v0.306): a case-expression used in
                // statement position — its arm body is `Console.WriteLine(...)`
                // or any other void-returning call — lowers to a null `v`
                // (the void sentinel at LlvmBackend_Exprs.cpp:90). sema does
                // not record an exprType on the case expression when every
                // arm body is Unit/void (§19.2's "no common type, no
                // recording" rule), so resultSemaTy is null and resultTy
                // defaults to i32 above — `resultTy->isVoidTy()` is false.
                // Without this path the `if (!v) return nullptr` here bails
                // out of lowerCaseExpr BEFORE the deferred
                // `CreateBr(endBB)` loop fires, leaving `case.end` with
                // no predecessors and the second-and-later arms never
                // processed — the IR verifier rejects `case.end` as
                // unterminated. Promote `resultTy` to void for this case
                // expression and proceed exactly like the explicit-void
                // arm above. (The PHI construction is skipped at the end
                // by the existing post-loop `resultTy->isVoidTy()` guard.)
                if (!resultSemaTy && !resultTy->isVoidTy()) {
                    resultTy = llvm::Type::getVoidTy(llvm_);
                    incomings.push_back(
                        {nullptr, builder_.GetInsertBlock()});
                    diverged_ = false;
                    builder_.SetInsertPoint(nextBB);
                    continue;
                }
                // fix #5032 (§14.1 / §14.2 / §19.3, v0.307): the case has a
                // CONCRETE, non-void result type (`resultSemaTy` set) but
                // this arm's body still failed to lower — the shape neither
                // exemption above covers. This is the DEAD unspecialized
                // generic-method-template body (issue #490): a `where T:
                // IShapeN`-constrained method's template emits T erased to a
                // non-pointer placeholder, so a type-test arm over `v: T`
                // (`var q: SqN => q.Area() + 10`) has patternMatches decide
                // the arm statically never matches — but the arm BODY is
                // still lowered unconditionally to keep the template's IR
                // complete, and `q.Area()` hits emitMethodCall's #484
                // non-pointer-receiver guard and returns null. Before this
                // fix that fell straight through to `return nullptr` here,
                // which — UNLIKE the value-demanding sites this mirrors
                // (#2434/#2491/#1386) — is not merely a missing value: this
                // arm's `bodyBB`/`nextBB` blocks and the shared `case.end`
                // block (built at function entry) are already live in
                // `currentFn_` and this bails out WITHOUT terminating any of
                // them, so the IR verifier rejects `case.end` as having no
                // terminator with NO diagnostic at all (a silent internal
                // crash, not even the #1386 ICE text). Only the SPECIALIZED
                // instantiation ($spec$SqN) is ever called, and there the
                // receiver is a real pointer and `q.Area()` dispatches
                // normally. Mirror the LAMBDA-body precedent for this exact
                // shape (LlvmBackend_Exprs2.cpp's #1255 fix): degrade to a
                // well-typed zero value for `resultTy` and keep going, so
                // every block this arm needs gets its terminator — same
                // three-outcome severity split as the sibling sites, reusing
                // their shared predicate rather than a new copy of it.
                std::string member, appliedMember;
                const std::string builtinRecv = builtinReceiverOfMemberCall(
                    arm.exprBody.get(), &member, &appliedMember);
                if (!builtinRecv.empty() && !ctx_.diagnostics().hasErrors()) {
                    ctx_.diagnostics().report(
                        Severity::Error, arm.exprBody->span.begin,
                        "`" + member + "` on built-in type `" + builtinRecv +
                        "` produced no value, so it cannot be this `case` "
                        "arm's result. `" + builtinRecv +
                        "` has no user declaration, so its member surface "
                        "is exactly what the standard library documents "
                        "for it: either `" + member + "` is not on that "
                        "surface (a misspelling), or it is documented but "
                        "yields nothing here (#2935 \xE2\x80\x94 if it is "
                        "documented and should yield a value, it is "
                        "unimplemented, #2457)" +
                        builtinMemberChainNote(member, appliedMember));
                } else if (!ctx_.diagnostics().hasErrors()) {
                    ctx_.diagnostics().report(
                        Severity::Warning, arm.exprBody->span.begin,
                        "internal: case-arm expression body failed to "
                        "lower to a value (degraded to a zero/null arm "
                        "result) — #1386");
                }
                v = llvm::Constant::getNullValue(resultTy);
            }
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
            // fix #1934 / §19.2 / §28.3 (producers arm-meet, v0.184) / §28.9:
            // an expression-position `case`/`when` whose result is a
            // (non-nullable) owned `string`. §28.3's producers table makes the
            // result the arm-meet of the arms — for a runtime-managed `string`
            // (§28.9) that means the result must carry EXACTLY +1 (one reference
            // the consumer discharges once), on WHICHEVER arm is taken. Because a
            // PHI joins the arms into a single value, the consumer (return /
            // assign / bind) can only make ONE retain-or-transfer decision for
            // the merged value, so each arm must be normalized to +1 HERE, in the
            // arm's own block, before it reaches the result PHI — mirroring the
            // per-expression normalization the ReturnStmt / Assign paths already
            // apply to a plain `return <concat>` (which is why that form is clean
            // while this one leaked). An arm body that is itself a +1 producer
            // (`+`/interpolation/nested string-returning call, or a string local
            // owning +1) already carries its single reference — leave it; any
            // other arm (a param/field borrow, a string-literal sentinel) is a +0
            // borrow and is RETAINED to mint the +1 (bz_string_retain is a no-op
            // on a sentinel/literal/null). The whole case-expression is then a
            // single +1 producer (isPlusOneStringProducer recognizes it), so the
            // consumer TRANSFERS the reference out without a second retain. Before
            // this, a producer arm's +1 was double-counted by the consumer's
            // borrow-retain — the arm's original reference leaked (#1934); and
            // recognizing the case-expression as a producer WITHOUT this per-arm
            // mint would drop a borrow arm's reference (a use-after-free).
            //
            // fix #1951 (conditional escape): RETAIN a bare read of a named
            // string LOCAL that owns +1 (a producer/borrow local) written as an
            // arm — do NOT transfer it (the removed `!returnedStringLocalOwns
            // PlusOne` guard). A `case`/`when` arm `1 => s` selects `s` on ONE
            // path only, so transferring `s`'s single reference out (leaving the
            // slot's +1 uncounted) leaked `s` on EVERY OTHER path, where the
            // suppressed scope-end release never ran (#1951). Retaining instead
            // mints a fresh +1 for the arm result and leaves `s`'s own +1 in its
            // slot, so the coordinated StringEscapeWalker relaxation (a bare
            // string-typed case-arm read is a borrow, so the local stays scope-
            // end-release eligible) discharges `s` once on the non-selected paths
            // and the retained arm reference once on the selected path — balanced
            // on every path, no leak, no double-free. This diverges from the
            // ReturnStmt/arrow/lambda paths, which keep the transfer for an
            // UNCONDITIONAL escape (`return s`) where the single reference
            // genuinely flows out and the local is not released.
            //
            // NULLABLE-INCLUSIVE (fix #2200, nullable analog of #1951). After #2196
            // the whole `string?` value path is refcounted (callee return-mint
            // + caller producer recognition + caller scope-end release), so
            // the pre-#2196 unsoundness ("minting +1 for a `string?` arm-meet
            // leaks because the nullable return path is un-refcounted") is
            // gone. Extending the per-arm mint + the StringEscapeWalker
            // relaxation (coordinated, both keyed on the SAME nullable-string-
            // result test — see isStringResultArmMeet below) to nullable
            // closes the leak the conservative pre-#2196 posture caused:
            // a `case n of 1 => s, _ => null` written where `s` is a +1
            // string local leaked `s`'s +1 on every non-selected path
            // (transfer-style suppression of scope-end release combined with
            // no arm-side mint = unbalanced leak). The same mint+walker-
            // relaxation pair that makes the non-nullable arm-meet balanced
            // makes the nullable one balanced; bz_string_retain is null-safe,
            // so the `null` arm simply no-ops. // per #1951, #2200, §28.3,
            // §28.9
            if (v && resultSemaTy &&
                resultSemaTy->kind == sema::TypeRef::Kind::Primitive &&
                resultSemaTy->primitive == sema::PrimitiveType::String &&
                v->getType()->isPointerTy() &&
                !isPlusOneStringProducer(arm.exprBody.get()))
                emitStringRetain(v);
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
            // fix #1952 / §7.1 / §7.2 / §28.9: reconcile ONLY a genuine LLVM
            // TYPE mismatch — a ptr-null arm alongside a NON-POINTER value arm
            // (`1 => 100, _ => null`, where `100` is a bare i32 and `null` is a
            // ptr). A NON-NULL POINTER arm (a class/`string` reference) shares
            // the `ptr` type with the ptr-null arm, so a nullable REFERENCE
            // result (`string?`, `Object?`, `C?`) needs NO wrapping — it is just
            // a `ptr` whose null is a real null pointer. The prior classification
            // set `hasNonPtr` for ANY non-null arm, so a `1 => "a"+"b", _ =>
            // null` (result `string?`) was wrongly wrapped into a `{i8, ptr}`
            // tagged struct — the tagged form nullable VALUE types use. That
            // struct then escaped `isNullableValueTy(string?)==false`'s Path A,
            // survived the plain-`var` (inferred-type) local path (which, unlike
            // the declared-type path's unwrapNarrowedNullable, does not unwrap
            // it), and left the minted arm string un-refcounted → a leak (#1952,
            // inferred `var s := (case … _ => null)` shape). lowerConditional's
            // Path B already gates on the genuine mismatch (`tv->getType() !=
            // ev->getType()`) — the if-expression sibling was clean for exactly
            // this reason; this brings the case/when path into line with it.
            bool hasNullPtr = false, hasNonPtr = false;
            llvm::Value* nonPtrSample = nullptr;
            for (auto& [v, bb] : incomings) {
                // fix #4469: void-returning arms contribute a null `v` (the
                // void sentinel); they have no LLVM type and skip the
                // widen-vs-mismatch classification entirely. The void-IR
                // branch at line 11213 below handles them.
                if (!v) continue;
                if (isNullPtr(v)) { hasNullPtr = true; }
                else if (!v->getType()->isPointerTy()) {
                    // A genuine non-pointer VALUE arm (bare scalar/struct) — the
                    // only kind that needs the tagged {i8, T} reconciliation.
                    hasNonPtr = true; nonPtrSample = v;
                }
                // else: a non-null POINTER arm — same `ptr` type as ptr-null,
                // no wrapping needed (a nullable reference stays a bare ptr).
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
    llvm::PHINode* phi = nullptr;
    if (incomings.empty()) {
        // CG40: every arm diverged — endBB has no predecessors and the caller
        // emits nothing into it; an empty block is malformed IR. Cap with
        // `unreachable` and report divergence, exactly as lowerConditional does.
        builder_.CreateUnreachable();
        diverged_ = true;
        return llvm::PoisonValue::get(resultTy);
    }
    // fix #4469: a unit/void-typed case expression (§4.7) has every arm
    // yielding a null `v` (the void sentinel from lowerExpr's
    // normalization at LlvmBackend_Exprs.cpp:90) and the case's §20
    // result type is `void` — void is NOT a first-class type and cannot
    // form a PHI. Skip PHI construction AND the resultSlot/scrutEndBB
    // dance (they assume a non-void PHI result to thread through a slot
    // for the dispose-call register-clobber); emit the normal-edge
    // scrut dispose and a fresh scrutEndBB terminator if there IS an
    // owned scrutinee (the dispose still runs), then return nullptr.
    // endBB has predecessors from the deferred-branch loop above, so the
    // IR is well-formed — the case-expression-as-statement-position
    // ExpressionStmt shape the issue measured.
    if (resultTy && resultTy->isVoidTy()) {
        if (ce.scrutinee && scrutOwnedSlot) {
            llvm::Value* scrutVal = builder_.CreateLoad(
                ptrTy(), scrutOwnedSlot, "casescrut.load");
            if (!scrutOwnIsOwningCollection)
                emitDisposeAndReclaim(scrutVal);
            else
                freeUnboundOwnedOwningCollectionTemp(scrutVal, scrutTy);
            if (!disposeStack_.empty()) {
                auto& scope = disposeStack_.back();
                for (auto it = scope.begin(); it != scope.end(); ++it) {
                    if (it->slot == scrutOwnedSlot) {
                        scope.erase(it);
                        break;
                    }
                }
            }
            auto* scrutEndBB = llvm::BasicBlock::Create(
                llvm_, "casescrut.end", currentFn_);
            builder_.CreateBr(scrutEndBB);
            builder_.SetInsertPoint(scrutEndBB);
        }
        // fix #4625: a unit/void-result case expression forms no PHI, so the
        // reclaim has no ordering constraint here — emit it at the top of
        // `case.end`, exactly where `lowerCaseStmt` emits its own.
        emitOwningChoiceScrutReclaim();
        return nullptr;
    }
    // K8 full-expression boundary (#1795): dispose the case-scrutinee owned
    // temporary on the normal edge. The scrut slot holds a fresh non-null
    // pointer (always populated by the store registered above) so the dispose
    // is unconditional here — paired with the `unwindOnly` Disposal entry on
    // the dispose stack that fires on an arm-body throw, the two paths are
    // mutually exclusive. The function's original endBB content (PHI + return)
    // lives in a fresh `scrutEndBB`, whose single predecessor is endBB, so
    // the PHI predecessors stay correct and IR verification passes.
    llvm::BasicBlock* scrutEndBB = nullptr;
    llvm::AllocaInst* resultSlot = nullptr;
    if (ce.scrutinee && (scrutOwnedSlot || scrutChoiceSlot)) {
        // Allocate a slot for the case's result value BEFORE endBB has any
        // instructions, so the dispose call below can run without first having
        // to thread the result through a PHI (the PHI in endBB would otherwise
        // have its predecessors broken when we route through a fresh block).
        resultSlot = entryAlloca(incomings[0].first->getType());
    }

    phi = builder_.CreatePHI(incomings[0].first->getType(),
                             incomings.size());
    for (auto& [v, bb] : incomings) phi->addIncoming(v, bb);
    if (ce.scrutinee && scrutOwnedSlot) {
        // Store the case result so scrutEndBB (endBB's single successor) can
        // re-load it after the dispose clobbers register state.
        builder_.CreateStore(phi, resultSlot);
        llvm::Value* scrutVal = builder_.CreateLoad(
            ptrTy(), scrutOwnedSlot, "casescrut.load");
        if (!scrutOwnIsOwningCollection)
            emitDisposeAndReclaim(scrutVal);
        else
            freeUnboundOwnedOwningCollectionTemp(scrutVal, scrutTy);
        if (!disposeStack_.empty()) {
            auto& scope = disposeStack_.back();
            for (auto it = scope.begin(); it != scope.end(); ++it) {
                if (it->slot == scrutOwnedSlot) {
                    scope.erase(it);
                    break;
                }
            }
        }
        scrutEndBB = llvm::BasicBlock::Create(
            llvm_, "casescrut.end", currentFn_);
        builder_.CreateBr(scrutEndBB);
        builder_.SetInsertPoint(scrutEndBB);
        return builder_.CreateLoad(
            incomings[0].first->getType(), resultSlot, "case.result");
    }
    // fix #4625 / §35.5 + §28.3 (v0.307): THE ORDERING CONSTRAINT expression
    // position adds over statement position. The reclaim must run AFTER the
    // selected arm's result value exists — otherwise it frees a payload the
    // result still borrows (§28.3: the boundary "is what keeps §19.3's pattern
    // bindings … alive through the arm body") — and it CANNOT be emitted
    // before the PHI, because a PHI must be the first instruction of its block
    // and `emitOwningChoiceLocationDisposal` emits a tag switch that TERMINATES
    // the current block and leaves the insert point in a fresh join block.
    //
    // Both are satisfied by: build the PHI at the top of `case.end` (each arm's
    // value was already computed and coerced in the arm's OWN block, so every
    // incoming edge dominates), spill it to an entry-block slot, emit the
    // reclaim, and reload in whatever block the reclaim left us in. That is the
    // same spill/reload the #1795 owned-POINTER scrutinee above performs, and
    // it keeps the returned value defined in the post-reclaim block without
    // disturbing the PHI's predecessor edges.
    if (ce.scrutinee && scrutChoiceSlot && scrutChoiceRef) {
        builder_.CreateStore(phi, resultSlot);
        emitOwningChoiceScrutReclaim();
        return builder_.CreateLoad(
            incomings[0].first->getType(), resultSlot, "case.result");
    }
    return phi;
}


// §17.15 / K8.5 (v0.216, #2098): the binding atom `(var x := E; body)`.
// Evaluation is straight-line and textual: clauses first (each a §21.1
// declaration lowered by the shared lowerLocalDecl — alloca + init store +
// locals_ registration, keyed by the clause's own LocalDecl node so an outer
// same-named local's slot is never clobbered), body last, then the atom's OWN
// disposal boundary. The atom pushes a dedicated dispose scope: a clause the
// ownership pass flagged bindAtomDisposeAtEnd (an owned production left
// undischarged inside the atom) is registered there right after its
// establishment, so
//   * the NORMAL edge disposes at the atom's completion, after the body's
//     value is computed, in reverse clause order (emitDisposals iterates the
//     scope in reverse registration order — K8.5's "after the body's
//     evaluation, in reverse clause order");
//   * the EXCEPTION edge gets K3.15 parity for free — the scope sits on
//     disposeStack_, so every emitPendingCheck emitted while lowering later
//     clauses or the body disposes it during unwinding on the same
//     establishment-ordered cleanup stack;
//   * a clause CONSUMED inside the atom (a K3 position discharged it) is
//     never flagged, so the boundary disposal does not run for it — the
//     consume-site machinery is the sole discharge (no double-free).
// The body's value is computed BEFORE the boundary disposals run; a body
// whose value would borrow an owned atom binding is rejected in the
// ownership pass (K8.4 at the atom boundary), so the returned value never
// dangles on the disposed bindings.
llvm::Value* Emitter::lowerBindExpr(const ast::BindExpr& bx) {
    disposeStack_.push_back({});
    for (std::size_t i = 0; i < bx.clauses.size(); ++i) {
        const auto& cl = bx.clauses[i];
        if (!cl) continue;
        lowerLocalDecl(cl.get(), cl->initializer.get(), cl->type.get());
        if (diverged_) {
            // A diverging clause initializer (a `throw` form): the unwind
            // edge disposed the scope's registered entries; nothing more
            // evaluates.
            disposeStack_.pop_back();
            return llvm::PoisonValue::get(ptrTy());
        }
        // K8.5: the atom-boundary disposal, registered at establishment so
        // the unwind edge covers the window between this clause and the
        // atom's completion. The decision is the ONE shared predicate the
        // ownership pass cross-checks (common::bindClauseNeedsAtomDisposal
        // — owned production, not discharged by a K3 position inside the
        // atom), so the two passes cannot split into a leak or a
        // double-free; registerUsingDisposal then selects the correct
        // Disposal kind for the clause's value (class/interface dynamic
        // dispatch, owning collection cascade, …) exactly as a `using var`
        // would be registered — the K8.5 boundary is the atom's scope
        // instead of the statement's.
        if (common::bindClauseNeedsAtomDisposal(
                bx, i, ctx_,
                [this](const sema::TypeRef* t) { return resolveTypeParamRef(t); }))
            registerUsingDisposal(cl.get());
    }
    llvm::Value* v = bx.body ? lowerExpr(*bx.body) : nullptr;
    if (diverged_) {
        // A diverging body (`(var s := new X(); throw e)`): the throw's
        // unwind edge already disposed this scope's entries during
        // unwinding (K3.15) — pop without a normal-edge emission and
        // propagate the divergence.
        disposeStack_.pop_back();
        return v ? v : llvm::PoisonValue::get(ptrTy());
    }
    // The atom's completion: dispose undischarged owned bindings on the
    // normal edge, after the body's evaluation, in reverse clause order.
    emitDisposals(disposeStack_.back(), /*isUnwind=*/false);
    disposeStack_.pop_back();
    return v;
}


// §19.3/§19.5 (#1938): a class/interface/exception type-test lowers to emitIsA,
// which loads obj[0] and dereferences it as a type descriptor. When an `Object`
// (or interface reference) scrutinee actually holds a vtable-less `bz_string*`,
// obj[0] is the string's `char* data`, not a descriptor — walking it faults
// (SIGSEGV). Guard the is-a: when the static scrutinee type COULD hold a runtime
// string (`Object` or an interface), first ask bz_obj_is_string and short-
// circuit a string to a clean never-match (a string is never a class/interface/
// exception); otherwise emit the plain is-a. This mirrors the guard
// emitTypeParamPatternTest::guardedIsA already applies for the type-PARAMETER
// pattern form (#1852) — the concrete and type-parameter paths stay consistent.
// The guard is a no-op for a statically class/interface/exception-typed
// scrutinee (mayBeString false), so it never disturbs class/exception dispatch.
llvm::Value* Emitter::emitStringGuardedIsA(llvm::Value* scrut,
                                           llvm::GlobalVariable* desc,
                                           const sema::TypeRef* scrutTy) {
    bool mayBeString =
        !scrutTy ||
        (scrutTy->kind == sema::TypeRef::Kind::Primitive &&
         scrutTy->primitive == sema::PrimitiveType::Object) ||
        (scrutTy->kind == sema::TypeRef::Kind::Named && scrutTy->declaration &&
         scrutTy->declaration->kind == ast::TypeDeclKind::Interface);
    if (!mayBeString) return emitIsA(scrut, desc);
    llvm::BasicBlock* preBB = builder_.GetInsertBlock();
    auto* isStrFn = declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
    llvm::Value* isStr = builder_.CreateICmpNE(
        builder_.CreateCall(isStrFn, {scrut, vtableTablePtr()}),
        builder_.getInt32(0));
    auto* doIsaBB = llvm::BasicBlock::Create(llvm_, "pm.isa", currentFn_);
    auto* joinBB = llvm::BasicBlock::Create(llvm_, "pm.isa.join", currentFn_);
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

llvm::Value* Emitter::patternMatches(const ast::Pattern* pat, llvm::Value* scrut,
                            const sema::TypeSymbol* enumTy,
                            bool isStr,
                            const sema::TypeRef* scrutTy) {
    if (dynamic_cast<const ast::DiscardPattern*>(pat))
        return builder_.getInt1(true);

    // §35.5 (v0.200, #2045): a CHOICE scrutinee. The scrutinee's resolved
    // type is a choice (a value-typed sum lowered as `{ i32 tag, … }` per
    // choiceStruct); match is by tag, not by value equality. Two pattern
    // shapes reach here:
    //   * NamedPattern (bare case name) — matches when tag == caseIndex.
    //     A payload-carrying case bare-named is a sema error (rejected by
    //     checkTopLevelNamedPattern), so the bare form here is always a
    //     payload-free case.
    //   * DestructuringPattern (CaseName(slots...)) — matches when
    //     tag == caseIndex AND every slot's own sub-pattern matches the
    //     extracted payload field. A naked binding or `var x` slot binds
    //     unconditionally (the dominant shape — `Some(var v)`, `First(a,
    //     b)`); any other slot kind (constant, range, relational,
    //     alternative, nested destructuring, ...) is RECURSED into via
    //     patternMatches and ANDed into the result, mirroring the RECORD-
    //     scrutinee DestructuringPattern branch below. fix #3902 (v0.294):
    //     until this fix, every non-binding slot was silently skipped and
    //     `matched` reduced to pure tag equality, so ANY refutable payload
    //     slot pattern always matched regardless of the payload's actual
    //     value — e.g. `case w of Val(5, 1) => …` matched `Val(5, 99)`.
    // A choice's NamedPattern never resolves through the enum-constant or
    // type-test legs below (choice cases carry no enumValue_ entry and no
    // descriptor), so this early branch is the only path that admits them.
    // fix #4399 (v0.298): guard against nullable choice scrutinees — a
    // `MyEnum?` type has scrutTy->declaration->kind==Choice, but the
    // non-nullable path extracts field 0 of the LLVM value as the choice tag
    // (i32). For a nullable scrutinee the LLVM value is {i8, ChoiceStruct},
    // so field 0 is the i8 hasValue flag, causing "icmp eq i8, i32" IR
    // verification failure. Route nullable choice scrutinees through the
    // isNullableVal path below where nullablePrimValue extracts the inner
    // ChoiceStruct first.
    if (scrutTy && !scrutTy->nullable &&
        scrutTy->kind == sema::TypeRef::Kind::Named &&
        scrutTy->declaration &&
        scrutTy->declaration->kind == ast::TypeDeclKind::Choice) {
        const sema::TypeSymbol* choiceTy = scrutTy->declaration;
        // NamedPattern: payload-free case by bare name (`None => …`).
        if (auto* np = dynamic_cast<const ast::NamedPattern*>(pat)) {
            if (np->name && !np->name->parts.empty()) {
                std::size_t tag = 0;
                if (auto* cc = choiceCaseByName(choiceTy,
                                                np->name->parts.back(), &tag)) {
                    (void)cc;
                    llvm::Value* tagVal =
                        builder_.CreateExtractValue(scrut, {0}, "choice.tag");
                    return builder_.CreateICmpEQ(
                        tagVal, llvm::ConstantInt::get(i32(), tag),
                        "choice.tag.eq");
                }
            }
        }
        // DestructuringPattern: payload case by `CaseName(slots...)`.
        if (auto* dp = dynamic_cast<const ast::DestructuringPattern*>(pat)) {
            if (dp->typeName && !dp->typeName->parts.empty()) {
                std::size_t tag = 0;
                if (auto* cc = choiceCaseByName(
                        choiceTy, dp->typeName->parts.back(), &tag)) {
                    // §14.1 / §28.1 (v0.226, #2208): a GENERIC-choice match
                    // arm must extract the payload at the SPECIALIZED LLVM
                    // type — the choice struct returned by choiceStruct is
                    // keyed on the active substitution (per #2047 the
                    // construction site already installs the guard here).
                    // Without this guard a `Some(value: T)` arm with T =
                    // Array<int> extracts from the unspecialized {i32}
                    // payload slot, reads zero/garbage into the binding's
                    // local, and the program's `inner.Count` returns 0.
                    // The owning-choice active-case disposal
                    // (LlvmBackend_Disposal.cpp #2047) and the value-like
                    // string release (also #2047) install the same guard;
                    // this arm-binding path was the asymmetric omission.
                    RecordSubstGuard choiceMatchSubst(this, scrutTy);
                    llvm::Value* tagVal =
                        builder_.CreateExtractValue(scrut, {0}, "choice.tag");
                    llvm::Value* matched = builder_.CreateICmpEQ(
                        tagVal, llvm::ConstantInt::get(i32(), tag),
                        "choice.tag.eq");
                    // Bind the case's payload fields. The case's payload
                    // slot is at field index `1 + caseIndex` in the choice
                    // struct; each payload field is a sub-field of that
                    // struct (in declaration order). Naked and `var x`
                    // sub-patterns install a local slot keyed on the same
                    // AST node sema used (mirrors the record-destructure
                    // binding in the DestructuringPattern handler below).
                    if (cc->caseDecl) {
                        for (std::size_t i = 0;
                             i < dp->subPatterns.size() &&
                             i < cc->caseDecl->payload.size();
                             ++i) {
                            const auto& sp = dp->subPatterns[i];
                            llvm::Value* slotStruct =
                                builder_.CreateExtractValue(
                                    scrut, {1 + (unsigned)tag},
                                    "choice.case.slot");
                            llvm::Value* fv =
                                builder_.CreateExtractValue(
                                    slotStruct, {(unsigned)i},
                                    "choice.case.field");
                            if (!sp.nakedBinding.empty()) {
                                llvm::AllocaInst* slot =
                                    entryAlloca(fv->getType());
                                builder_.CreateStore(fv, slot);
                                // §19.3 / §27.5: a case-arm binding owns its
                                // capture-cell lifetime at the arm scope, not at
                                // the nested block that first captures it.
                                if (!disposeStack_.empty())
                                    recordDeclScopeDepth(  // fix #4523
                                        sp.nakedAnchor.get());
                                locals_[sp.nakedAnchor.get()] = slot;
                            } else if (auto* vbp =
                                           dynamic_cast<const ast::BindingPattern*>(
                                               sp.pattern.get())) {
                                llvm::AllocaInst* slot =
                                    entryAlloca(fv->getType());
                                builder_.CreateStore(fv, slot);
                                // §19.3 / §27.5: a case-arm binding owns its
                                // capture-cell lifetime at the arm scope, not at
                                // the nested block that first captures it.
                                if (!disposeStack_.empty())
                                    recordDeclScopeDepth(vbp);  // fix #4523
                                locals_[vbp] = slot;
                            } else if (sp.pattern) {
                                // fix #3902 / §35.5: a REFUTABLE payload slot
                                // (constant, range, relational, alternative
                                // list, nested destructuring, ...) must be
                                // CHECKED against the extracted field value,
                                // not silently admitted. Recurse via
                                // patternMatches exactly the way the RECORD-
                                // scrutinee DestructuringPattern branch below
                                // does (~L11670+: `patternMatches(sp.pattern
                                // .get(), fieldVal, subEnumTy, subIsStr,
                                // fieldTy)`), so a slot pattern that is
                                // itself a nested DestructuringPattern both
                                // tests (tag/type match) and binds its own
                                // sub-slots — the recursive call handles
                                // both halves uniformly.
                                //
                                // Resolve the slot's declared payload type,
                                // substituted through the scrutinee's actual
                                // instantiation (the same helper the owning-
                                // choice disposal codegen uses for the
                                // sibling §28.1 erasure decision,
                                // LlvmBackend_Disposal.cpp ~L2895), so an
                                // enum-constant NamedPattern and a string
                                // ConstantPattern sub-pattern get the same
                                // enumTy/isStr context a top-level pattern
                                // match would (mirrors fix #615's record-side
                                // rationale).
                                const sema::TypeRef* fieldTy =
                                    common::substitutedChoicePayloadType(
                                        cc->caseDecl->payload[i], choiceTy,
                                        scrutTy, ctx_);
                                const sema::TypeSymbol* subEnumTy = nullptr;
                                bool subIsStr = false;
                                if (fieldTy &&
                                    fieldTy->kind ==
                                        sema::TypeRef::Kind::Named &&
                                    fieldTy->declaration &&
                                    fieldTy->declaration->kind ==
                                        ast::TypeDeclKind::Enum) {
                                    subEnumTy = fieldTy->declaration;
                                } else if (
                                    fieldTy &&
                                    fieldTy->kind ==
                                        sema::TypeRef::Kind::Primitive &&
                                    fieldTy->primitive ==
                                        sema::PrimitiveType::String) {
                                    subIsStr = true;
                                }
                                llvm::Value* subMatch = patternMatches(
                                    sp.pattern.get(), fv, subEnumTy, subIsStr,
                                    fieldTy);
                                if (subMatch)
                                    matched =
                                        builder_.CreateAnd(matched, subMatch);
                            }
                        }
                    }
                    return matched;
                }
            }
        }
        // Other pattern kinds on a choice scrutinee are not supported
        // (fall through to the deferred-return-null path below; the
        // caller degrades the arm to no-match, never a miscompile).
    }

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
        return builder_.CreateICmpSGE(cmp, zero);
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
                         scrut && scrut->getType()->isStructTy();
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
            // fix #4399 (v0.298, §35.9): a NamedPattern on a nullable CHOICE
            // scrutinee (e.g. `None` on `Maybe?`) is a choice-case test: hasValue
            // AND the choice tag matches the case. The existing NamedPattern
            // handler above only covers enum constants; a bare choice-case name
            // (which resolves to the CaseDecl, not to an enum constant) fell
            // through to nullptr → lowerCaseExpr left `case.end` unterminated
            // (IR-verify crash). Mirror the non-nullable choice NamedPattern
            // handler above (lines 11011-11024): look up the name as a choice
            // case, extract the tag from innerVal (the unwrapped choice struct),
            // compare with the case's i32 tag, AND with hasValue.
            if (np->name && !np->name->parts.empty() &&
                scrutTy->kind == sema::TypeRef::Kind::Named &&
                scrutTy->declaration &&
                scrutTy->declaration->kind == ast::TypeDeclKind::Choice) {
                const sema::TypeSymbol* choiceTy = scrutTy->declaration;
                std::size_t tag = 0;
                if (auto* cc = choiceCaseByName(
                        choiceTy, np->name->parts.back(), &tag)) {
                    (void)cc;
                    llvm::Value* tagVal =
                        builder_.CreateExtractValue(innerVal, {0}, "choice.tag");
                    llvm::Value* matched = builder_.CreateICmpEQ(
                        tagVal, llvm::ConstantInt::get(i32(), tag),
                        "choice.tag.eq");
                    return builder_.CreateAnd(hasVal, matched, "nv.choice.match");
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
            // fix #4083 / §19.3 / §19.5 / §27.8 (v0.294): a `type` alias
            // whose underlying type is a NULLABLE primitive (`type X :=
            // int?;`), used in a bare NamedPattern position on a
            // nullable-VALUE scrutinee, is the NamedPattern-path twin of
            // the `TypePattern` "case x of int? => …" handled at L11115 —
            // both names resolve to the SAME nullable primitive, so the
            // test reduces to `hasVal` exactly when the value is present
            // (§19.5 "matches only non-null values"). The sibling shared
            // helper `sema::resolveAliasToTypeSymbol` rejects non-`Named`
            // `TypeRef::Kind`s (function, primitive, InlineArray, …), so
            // this site has its OWN nullable-primitive-aware lookup.
            // Without this arm the alias name fell through to the `return
            // nullptr` below — basic block unterminated, the LLVM IR
            // verifier crashed ("Basic Block … does not have terminator" +
            // the #2491 backstop; pre-existing on `eae547e`, NOT a #4076
            // regression — #4076's new arm correctly returns null/continue
            // for a non-`Named` `resolveAliasToTypeSymbol` result). For an
            // alias whose underlying kind is anything ELSE (function type,
            // non-matching primitive, primitive array, …), the same
            // #4083 root cause means we must NOT propagate `nullptr` either
            // — mirror the single-member NamedPattern #1847 backstop: emit
            // the §19.3 diagnostic once per arm and OR a never-match
            // (`getInt1(false)`) into the case so `case.end` stays
            // terminated and the rest of the arm chain still emits
            // correctly (the same shape the #2227 alt-list fix uses for
            // the adjacent type-mismatch case, but for a member that does
            // not lower at all rather than one that lowers to a mismatched
            // type). Without an alias at all, fall through to the existing
            // `return nullptr` — no shape in scope today reaches it through
            // this isNullableVal branch besides a bare NAMED-PRIMITIVE-
            // pattern match this branch already handled above, so the
            // remaining `nullptr` is genuinely-deferred here.
            if (np->name && !np->name->parts.empty() &&
                scrutTy->kind == sema::TypeRef::Kind::Primitive &&
                scrutTy->nullable &&
                scrutTy->primitive != sema::PrimitiveType::None) {
                const sema::TypeAliasSymbol* aliasSym = nullptr;
                for (const auto& sym : ctx_.symbols()) {
                    auto* as = dynamic_cast<const sema::TypeAliasSymbol*>(
                        sym.get());
                    if (as && as->name == np->name->parts.back()) {
                        aliasSym = as;
                        break;
                    }
                }
                if (aliasSym) {
                    auto* aliasDecl =
                        dynamic_cast<const ast::TypeAlias*>(aliasSym->decl);
                    const sema::TypeRef* underlying =
                        (aliasDecl && aliasDecl->aliasedType)
                            ? ctx_.resolvedType(*aliasDecl->aliasedType)
                            : nullptr;
                    if (underlying &&
                        underlying->kind ==
                            sema::TypeRef::Kind::Primitive &&
                        underlying->primitive == scrutTy->primitive) {
                        // fix #5183 (§19.3 / §19.1 / §27.8, v0.311): an alias
                        // is a pure synonym (§27.8), so the bare-NamedPattern
                        // arm lowers AS the aliased primitive's type pattern
                        // over a nullable-value scrutinee. §19.3's refutable
                        // null test of a present value applies: a `T` (or
                        // `T?`) pattern over a `T?` scrutinee matches exactly
                        // when the value is present (hasValue). Pre-#5183 this
                        // arm additionally required `underlying->nullable`
                        // to match `scrutTy->nullable`, which rejected a
                        // non-nullable alias on a nullable scrutinee with the
                        // #1847 / #4083-arm-2 `not supported by the backend`
                        // diagnostic — a backstop originally installed to
                        // prevent an IR-verify crash on the same-shape empty
                        // alt-list sibling (#4083), NOT a spec verdict (§27.8
                        // + §19.3 + §19.1 do not allow the rejection). Drop
                        // the `underlying->nullable` gate; the alias lowers
                        // as the aliased type's pattern regardless of the
                        // alias's own nullability. The non-primitive
                        // (function, InlineArray) fall-through still emits
                        // the diagnostic for the non-Primitive-kind case,
                        // preserving the `fn_alias_on_nullable_int_bare_rejected_not_crashed`
                        // sibling.
                        return hasVal;
                    }
                    ctx_.diagnostics().report(
                        Severity::Error, np->span.begin,
                        "the `case`/`when` pattern `" +
                        std::string(np->name->parts.back()) +
                        "` is not supported by the backend: it did not "
                        "resolve to a type, enum constant, or named "
                        "constant to match against (\xC2\xA7""19.3)");
                    return builder_.getInt1(false);
                }
            }
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
                // fix #4062 / §19.3 / §19.5 (v0.294): a ConstantPattern
                // member that fails to lower to any LLVM value at all.
                // Sema's `ExprTC::altPartsNameType` classifies a `|`
                // alternative-list member as a "constant" if it does NOT
                // resolve to a user TypeSymbol (a function-type alias, an
                // unbound extension-method reference, a primitive alias,
                // …) and `checkAltMembersConstant` lets it through to
                // codegen — codegen then has no value to lower for it.
                // The pre-fix `if (!cv) return nullptr;` propagated out of
                // `patternMatches` and left `case.end` unterminated — the
                // LLVM IR verifier crashed with "Basic Block … does not
                // have terminator" + the #2491 backstop (measured crash
                // signature IDENTICAL to #4048's own original repro for
                // the alias-of-class case; #4048 closed that one at sema
                // for `TypeAliasSymbol`-of-Named, this codegen site
                // survives because `resolveAliasToTypeSymbol`'s null-is-
                // not-a-Type contract — §27.8 + the #4048 doc comment —
                // means any alias whose underlying kind is Function,
                // Primitive, or InlineArray falls through into this
                // unguarded path). Mirror the single-member NamedPattern
                // #1847 backstop diagnostic AND the #2227 alt-list "OR a
                // never-match `false` into `any`" pattern: emit the §19.3
                // diagnostic ONCE per arm (first bad member only — keeps
                // compiler output clean when several bad members share a
                // pattern; same fire-at-most-once discipline #2227 uses)
                // and `continue` so the rest of the arm's other `|`
                // members still participate in `any`. The IR remains
                // well-formed; the wildcard arm or a later genuine member
                // selects the case. Closing this codegen hole kills the
                // WHOLE crash-class, not one symbol-kind at a time as
                // #4048's sema-side fix did.
                if (!cv) {
                    if (!cp->constants.empty() &&
                        c.get() == cp->constants.front().get())
                        ctx_.diagnostics().report(
                            Severity::Error, c->span.begin,
                            "a `|`-alternative member could not be "
                            "lowered to a value to match against the "
                            "scrutinee — the case pattern did not "
                            "resolve to a constant in scope "
                            "(\xC2\xA7""19.3)");
                    continue;
                }
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

        // ⚠ SLOT-PATH ONLY (v0.286, #3298; narrowed further v0.294, #3692).
        // §19.3 makes a relational or range pattern over a NULLABLE scrutinee
        // a compile error, and sema now rejects it at both `case` sites
        // (ExprTC::checkOrderingPatternsOverNullableScrutinee) AND, since
        // #3692, at a DESTRUCTURING SLOT whose declared type sema can resolve
        // directly — a non-generic record field, or a choice payload field of
        // any instantiation (ExprTC::checkOrderingPatternOverNullableType,
        // fed from all six slot-type-resolution call sites in
        // ExprTypecheck.cpp / ExprTypecheck_Exprs.cpp: CaseExpr x CaseStmt,
        // record x choice, top-level x nested).
        //
        // #3692 did not close the gap for a GENERIC RECORD field typed at a
        // bare type parameter (`record Box<T>(X: T, Y: int)` instantiated as
        // `Box<int?>`) — there was no record analogue of the choice path's
        // generic-payload substitution. Fix #3899 (§14.1/§19.3, v0.294)
        // closed the CODEGEN side of that gap: the DestructuringPattern slot
        // loop below installs its own `RecordSubstGuard` (`destrSubst`, ~line
        // 11590) BEFORE computing any slot's field type — a different guard
        // from `armSubst` (~line 9771, for the arm BODY, fix #1254) — but the
        // three slot-type call sites read the bare, unsubstituted
        // `recordMemberType` instead of `recordMemberTypeResolved` (the
        // accessor that actually consults `destrSubst`), so `Box<int?>`'s
        // slot-0 type was seen as non-nullable `T`, and the `isNullableVal`
        // test just above this comment took the PLAIN (non-nullable) dispatch
        // — an IR-verification failure (`icmp {i8,i32}, i32`), not a clean
        // diagnosis. #3899 swapped all three call sites to
        // `recordMemberTypeResolved`.
        //
        // Fix #3898 (landed shortly after #3899, same revision) then closed
        // the SEMA side for the TOP-LEVEL instance of this shape
        // (`substituteGenericFieldType`, ExprTypecheck.cpp), so
        // `case b of Box(1..5, 1) => …` over a `Box<int?>` scrutinee is
        // rejected before it ever reaches codegen today
        // (`generic_record_slot_range_pattern_rejected_3898` in
        // case_relop_nullable_1074_test.cpp). #3898 does NOT reach a
        // GENERIC record slot NESTED inside another destructuring pattern
        // (`Outer<T>(B: Box<T>, …)`, or the same nesting under a choice-case
        // payload) — `bindNestedDestructuringNames` has no substitution
        // context there (issue 3938, closed by fix #3938: the four caller
        // loops now thread each pending nested pattern's already-substituted
        // scrutinee TypeRef through the worklist, and the walk re-substitutes
        // the next level's slot types against that scrutTy) — so THAT shape
        // is also rejected at sema, symmetric with the top-level
        // (`generic_record_slot_nested_pattern_rejected_3938` /
        // `_in_choice_payload_rejected_3938` in the same test file).
        //
        // ⚠ CONFIRMED UNGUARDED (measured while reviewing #3692, BEFORE
        // #3899/#3898 landed): with both handlers below stubbed to
        // unconditional `return nullptr;`, the ENTIRE existing test suite —
        // every #3298/#3692 row, five adjacent reachable-code-path sweeps,
        // and the whole lit corpus — stayed green. At that time no admitted
        // program reached either handler. After #3899 landed but before
        // #3938, the nested generic-record shape above was a real,
        // admitted program that reached them. After #3938 closed the sema
        // side, no admitted program reaches either handler — the tree is
        // back to the pre-#3899 state.
        //
        // Deleting these two handlers regardless is NOT a net deletion, it is
        // a silent miscompile FOR THE CASES sema does NOT reject: both slot
        // call sites do `if (subMatch) matched = AND(matched, subMatch);`, so
        // a nullptr return SKIPS the slot's test and the arm matches
        // unconditionally. MEASURED on the draft that deleted them (pre-
        // #3298): `R(1..5, 1)` against a NON-generic `R(99, 1)` returned the
        // arm's value instead of falling through — exactly the "→ AND
        // skipped → always matched" defect the slot code's own comment
        // records. No currently-admitted program reaches them after #3938,
        // but the handler MUST stay so a future regression that re-admits a
        // shape doesn't silently miscompile; the handlers are the SLOT-PATH-
        // ONLY authority, kept unconditionally. Do not delete them.
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
        // slots from the record's fields — then AND with hasValue.
        //
        // fix #4399 (v0.298): when the underlying type IS a choice (e.g.
        // `Maybe?` with a `Some(var v)` arm), passing scrutTy=null on the
        // recursion skips the choice handler (which requires scrutTy to identify
        // the choice type and emit the correct i32 tag constant), and instead
        // falls through to the record/other handler. The record handler then
        // extracts field 0 of the inner nullable wrapper `{i8 hasValue;
        // ChoiceStruct}` as i8 (the hasValue flag) and compares it with i32
        // 0 — an LLVM IR verification failure ("icmp eq i8, i32").
        //
        // The fix: when the nullable scrutinee's underlying type is a Named
        // choice, construct a non-nullable Named TypeRef pointing to the same
        // choice declaration and pass it to the recursive call so the choice
        // path fires with the correct i32 tag constant.
        if (dynamic_cast<const ast::DestructuringPattern*>(pat)) {
            const sema::TypeRef* recurseScrutTy = nullptr;
            // fix #3487 ratchet (v0.298): `choiceTy` must be at the same or
            // shallower scope as `recurseScrutTy` so that `recurseScrutTy = &choiceTy`
            // does not store the holder's address into a pointer that outlives it.
            // Previously declared inside the inner `if` (depth 5), causing a
            // stack-use-after-scope hit.  Hoisted to this outer scope (depth 4).
            static sema::TypeRef choiceTy;
            if (scrutTy && scrutTy->kind == sema::TypeRef::Kind::Named &&
                scrutTy->declaration &&
                scrutTy->declaration->kind == ast::TypeDeclKind::Choice) {
                choiceTy.kind = sema::TypeRef::Kind::Named;
                choiceTy.declaration = scrutTy->declaration;
                choiceTy.nullable = false;
                choiceTy.genericArgs = scrutTy->genericArgs;
                recurseScrutTy = &choiceTy;
            }
            llvm::Value* sub =
                patternMatches(pat, innerVal, enumTy, isStr, recurseScrutTy);
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
        // §19 / §A.8 (#957, #1853): a NamedPattern whose name resolves to a
        // named CONST — a local compile-time const (`const K := 7`, #1853), a
        // qualified `C.K`, or a bare module/own-class const — is a constant-
        // equality pattern, not a type test. The parser emits a NamedPattern
        // for any non-primitive name and defers Constant-vs-Type to resolution
        // here. Fold the const's value and compare (mirrors the ConstantPattern
        // handler: reconcile width for non-string scrutinees, content-compare
        // for strings). Checked before the type-descriptor branch; the two
        // never collide (a const's last name part is not a type name).
        if (llvm::Value* cv = constValueForPatternName(np)) {
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
                 scrutTy->primitive == sema::PrimitiveType::Object) ||
                // #1854 / §19.3 + §31: a BUILT-IN exception scrutinee (e.g. a
                // `catch e: Exception` binding) is a heap object carrying a
                // built-in vtable (setupBuiltinExceptions) whose vtable[0] is the
                // `btd$<name>` descriptor emitIsA walks — object-like exactly like
                // a class instance. It has a Named TypeRef with a NULL declaration
                // (built-ins have no user TypeSymbol), so the declaration-gated
                // class/interface disjunct above misses it. Without this, `case e
                // of InvalidOperationException => …` always emitted false and
                // silently fell through to `_` — a wrong-branch miscompile.
                isBuiltinExceptionTypeRef(scrutTy);
            if (!scrut->getType()->isPointerTy() || !objectLike)
                return builder_.getInt1(false);
            // #1938 / §19.3: an `Object`/interface scrutinee holding a string
            // would fault emitIsA (a string is a vtable-less bz_string*); guard
            // it to a clean never-match. No-op for a class/interface-typed
            // scrutinee. Consistent with the type-parameter path (#1852).
            return emitStringGuardedIsA(scrut, desc, scrutTy);
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
            // fix #4076 (§19.3/§27.8, v0.294): a `type` alias of a RECORD or
            // ENUM used as this same bare pattern shape (`AliasOfPt`)
            // resolves a symbol to a `sema::TypeAliasSymbol`, a distinct kind
            // the original `dynamic_cast<const sema::TypeSymbol*>` alone did
            // not recognize — the same independent-re-resolution gap #4050
            // fixed for the DestructuringPattern `considerSym` lambda a few
            // hundred lines above in this same function, and the class/
            // interface twin just above fixed via `descriptorForPatternName`
            // (LlvmBackend_Setup.cpp). Resolve through the shared
            // `sema::resolveAliasToTypeSymbol` (TypeResolution.hpp) so this
            // site shares one implementation with sema and its codegen
            // siblings rather than hand-rolling another copy. Matching is by
            // the RESOLVED TypeSymbol's identity (`scrutTy->declaration ==
            // ts`), not by name — an alias's own `name` is the alias's name,
            // not the aliased record/enum's, so re-checking `ts->name !=
            // last` after resolving through the alias would always fail.
            auto matchRecordOrEnumSym =
                [&](const sema::Symbol* s) -> const sema::TypeSymbol* {
                if (auto* ts = dynamic_cast<const sema::TypeSymbol*>(s))
                    return ts->name == last ? ts : nullptr;
                if (auto* aliasSym =
                        dynamic_cast<const sema::TypeAliasSymbol*>(s)) {
                    if (aliasSym->name != last) return nullptr;
                    return sema::resolveAliasToTypeSymbol(
                        ctx_, const_cast<sema::TypeAliasSymbol*>(aliasSym));
                    // #4047: null when the alias's aliasedType has not yet
                    // been resolved (declared textually before the type it
                    // names) — falls through to the next symbol / #1847.
                }
                return nullptr;
            };
            // fix #4079 / §19.5 / §2.8.3 rule 4 (v0.294): the local pass
            // below is module-local only — `ctx_.symbols()` is `symbols_`
            // (SemaContext.hpp:90), so an imported record/enum's TypeSymbol
            // (loaded into `importedSymbols_` by ModuleLoader) never
            // appears here, and a bare type-test pattern over an imported
            // record/enum (`case p of LibPt` after `import Lib;` declaring
            // `record LibPt`) falls through every arm to the #1847 backstop
            // diagnostic — a false reject of a program ExprTC's
            // `checkTopLevelNamedPattern` already accepts. Mirror the
            // established widening the #2845 fix applied to the class/
            // interface sibling `descriptorForPatternName` a few hundred
            // lines above in Setup.cpp: local pass first, then imported
            // pass over `ctx_.importedSymbols()`. The same widening the
            // #2844/#3197 fixes applied at sibling sites documented in
            // `descriptorForPatternName`'s comment
            // (LlvmBackend_Stmts.cpp:7657-7666, LlvmBackend_Assign.cpp:
            // 10008-10020). Orthogonal to the #4076 `TypeAliasSymbol`
            // arm — that arm added alias recognition INSIDE the local
            // loop; this widens the loop's scope.
            auto checkSym = [&](const sema::Symbol* s) -> llvm::Value* {
                const sema::TypeSymbol* ts = matchRecordOrEnumSym(s);
                if (!ts) return nullptr;
                if (ts->kind != ast::TypeDeclKind::Record &&
                    ts->kind != ast::TypeDeclKind::Enum) return nullptr;
                if (scrut->getType()->isPointerTy())
                    return builder_.getInt1(false);
                bool sameType = scrutTy && scrutTy->declaration == ts;
                // fix #4082 (§19.5 / §3.6 rule 4 / §27.8, v0.294): a bare
                // pattern resolved THROUGH A TYPE ALIAS that names a SPECIFIC
                // generic instantiation (e.g. `type AliasBoxStr := Box<string>;`
                // used as `case b of AliasBoxStr => …`) must match the
                // scrutinee on the alias's full instantiation, not on the
                // record declaration alone — `Box<int>` and `Box<string>` are
                // distinct runtime type identities under §3.6 rule 4 even
                // though they share the `Box` TypeSymbol*. The DIRECT
                // (unaliased) bare-name form has no spelling for type
                // arguments (#1206 — the pattern grammar carries no `<…>`
                // position), so its matches-any-instantiation behavior is
                // kept; the alias route is the ONE place a user can
                // communicate a specific instantiation through a bare
                // pattern, and the silent always-match otherwise computes
                // the wrong value. Compares `genericArgs` element-wise via
                // the shared `sema::typeRefEqual` so a `Box<int>` scrutinee
                // against a `Box<string>` alias falls through to the
                // next arm (the `_` in the issue's repro), as §19.5
                // requires.
                if (sameType && ts->kind == ast::TypeDeclKind::Record &&
                    scrutTy->kind == sema::TypeRef::Kind::Named) {
                    if (auto* aliasSym =
                            dynamic_cast<const sema::TypeAliasSymbol*>(s)) {
                        const auto* aliasDecl =
                            dynamic_cast<const ast::TypeAlias*>(
                                aliasSym->decl);
                        if (aliasDecl && aliasDecl->aliasedType) {
                            if (auto* underlying = ctx_.resolvedType(
                                    *aliasDecl->aliasedType)) {
                                if (scrutTy->genericArgs.size() !=
                                    underlying->genericArgs.size()) {
                                    sameType = false;
                                } else {
                                    for (size_t i = 0;
                                         i < scrutTy->genericArgs.size();
                                         ++i) {
                                        if (!sema::typeRefEqual(
                                                scrutTy->genericArgs[i],
                                                underlying->genericArgs[i])) {
                                            sameType = false;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                return builder_.getInt1(sameType);
            };
            for (const auto& sym : ctx_.symbols()) {
                if (llvm::Value* v = checkSym(sym.get())) return v;
            }
            for (const sema::Symbol* sym : ctx_.importedSymbols()) {
                if (llvm::Value* v = checkSym(sym)) return v;
            }
        }
        // §19.3 (v0.182, #1852): a bare TYPE-PARAMETER pattern `case x of T`. In
        // a monomorphized generic body a single-part name that resolves to none
        // of the handlers above may be a type parameter of the enclosing generic;
        // it tests the scrutinee against the SUBSTITUTED type argument, per
        // instantiation (the same §19.3 rule as `var x: T` and catch arms). The
        // active `genericSubstSemaTy_` maps the parameter name to the concrete
        // type for THIS instantiation; resolve it and emit the concrete type-test
        // through the shared helper (routed through #1843's per-monomorphization
        // descriptor for a constructed generic). Without this the name fell to the
        // #1847 diagnostic below (a clean but wrong "unsupported" error; a
        // malformed-IR ICE before #1847's guard landed).
        if (np->name && np->name->parts.size() == 1) {
            std::string_view pn = np->name->parts.back();
            auto sit = genericSubstSemaTy_.find(pn);
            if (sit != genericSubstSemaTy_.end() && sit->second) {
                // The substitution is already the concrete type. Emit the test;
                // if the helper cannot lower it, emit a TERMINATED never-match —
                // NEVER fall through to the #1847 diagnostic / return-nullptr,
                // which would leave `case.end` unterminated (#1852 blocker 1).
                if (llvm::Value* m = emitTypeParamPatternTest(
                        sit->second, scrut, scrutTy, np->span.begin))
                    return m;
                return builder_.getInt1(false);
            }
            if (emittingGenericTemplateBody_ &&
                templateGenericParamNames_.count(pn)) {
                // §19.3 (v0.182, #1852): the UNSPECIALIZED template body has no
                // active substitution, so T is unresolvable here. The template is
                // never called — every call binds a substitution and routes to a
                // specialization (where the real test above is emitted) — but it
                // must IR-verify. Emit a well-formed never-match so `case.end`
                // stays terminated (and no spurious #1847 error is reported for
                // the dead body).
                return builder_.getInt1(false);
            }
        }
        // §19.3 (#1847, #1853): a top-level NamedPattern that resolved to none
        // of the handlers above — an enum constant, a named `const` (member or
        // local — #1853 wired the local-const form into constValueForPatternName
        // above), a class/interface type descriptor, a record/enum type name, or
        // (above) a type parameter. Sema now rejects the reported ill-formed
        // forms (a bare identifier naming a runtime VALUE, a RUNTIME const, or an
        // unresolved name — checkTopLevelNamedPattern, ExprTypecheck.cpp), so a
        // pattern still reaching here is a valid-per-spec form the backend does
        // not yet lower. Returning nullptr here left `case.end` unterminated in
        // expression position (a malformed-IR ICE) and silently truncated the arm
        // chain in statement position (the mandatory `_` skipped — a silent
        // miscompile). Fail LOUDLY and consistently instead: emit a diagnostic
        // and a never-match so the block stays terminated.
        if (np->name && !np->name->parts.empty())
            ctx_.diagnostics().report(
                Severity::Error, np->span.begin,
                "the `case`/`when` pattern `" +
                std::string(np->name->parts.back()) +
                "` is not supported by the backend: it did not resolve to a "
                "type, enum constant, or named constant to match against "
                "(\xC2\xA7""19.3)");
        return builder_.getInt1(false);
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
            // fix #4062 / §19.3 / §19.5 (v0.294): sibling of the
            // nullable-value-scrutinee ConstantPattern guard directly
            // above (isNullableVal branch): a ConstantPattern member that
            // fails to lower to any LLVM value at all — same root cause
            // (sema's `altPartsNameType` lets a member that does NOT
            // resolve to a user TypeSymbol through to codegen, codegen
            // has no value to lower for it). The pre-fix `if (!cv)
            // return nullptr;` propagated out of `patternMatches` and
            // left `case.end` unterminated — the LLVM IR verifier
            // crashed with "Basic Block … does not have terminator" + the
            // #2491 backstop. Mirror the single-member NamedPattern
            // #1847 backstop diagnostic AND the #2227 alt-list "OR a
            // never-match `false` into `any`" pattern: emit the §19.3
            // diagnostic ONCE per arm and `continue` so the rest of the
            // arm's other `|` members still participate in `any`. Closing
            // this codegen hole at both `if (!cv) return nullptr;` sites
            // kills the WHOLE crash-class, not one symbol-kind at a time
            // as the parallel sema-side fix enumeration family (#4038,
            // #4048, #4050, #4051, #4052) does.
            if (!cv) {
                if (!cp->constants.empty() &&
                    c.get() == cp->constants.front().get())
                    ctx_.diagnostics().report(
                        Severity::Error, c->span.begin,
                        "a `|`-alternative member could not be lowered "
                        "to a value to match against the scrutinee — "
                        "the case pattern did not resolve to a constant "
                        "in scope (\xC2\xA7""19.3)");
                continue;
            }
            // #836: reconcile a non-literal constant's width to the scrutinee
            // (no-op for string / enum / already-matching types).
            if (!isStr) cv = reconcile(cv, scrut->getType());
            // fix #2227 / §19.3 / §19.5: a ConstantPattern member whose lowered
            // type is fundamentally incompatible with the scrutinee's LLVM type
            // (a `null` literal pointer over a non-nullable int/float/bool/enum
            // scrutinee — a `null` alternative is allowed by §19.3 but the
            // non-nullable scrutinee cannot match it; or a string-literal
            // `bz_string*` over an enum/record scrutinee whose typecheck guard
            // did not fire because `scrutTy->kind != Kind::Primitive` (e.g. a
            // bare `case c: Color of "Red" => …`). `reconcile` bridges only int
            // widths and FP widths, so the residual `ptr` vs `i32` mismatch
            // reaches `emitEquals` and trips `ICmpInst::AssertOK` (#2227
            // repros; a sibling of #964 / #1213 — those sema guards cover the
            // primitive-vs-primitive cases and the nullable-value `null`
            // member case, but miss these two). Mirror the NamedPattern
            // handler at L6422-6436: detect the fundamental mismatch, emit the
            // §19 diagnostic once per bad member, and OR a never-match (false)
            // into `any` so the rest of the arm's other members still
            // participate correctly. The diagnostic fires at most once per
            // pattern to keep the compiler output clean when the same
            // ill-formed pattern has several bad members.
            {
                llvm::Type* st = scrut->getType();
                llvm::Type* ct = cv->getType();
                bool incompatible = st != ct &&
                    (st->isPointerTy() != ct->isPointerTy() ||
                     st->isIntegerTy(1) != ct->isIntegerTy(1) ||
                     st->isFloatingPointTy() != ct->isFloatingPointTy());
                if (incompatible) {
                    if (!cp->constants.empty() && c.get() == cp->constants.front().get())
                        ctx_.diagnostics().report(
                            Severity::Error, c->span.begin,
                            "case pattern constant type is not compatible with "
                            "the scrutinee type (\xC2\xA7""19) — a `case` "
                            "constant must be comparable to the scrutinee");
                    // Skip this member — the OR-with-false leaves `any` unchanged
                    // for the remaining members.
                    continue;
                }
            }
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
    // §19: BindingPattern `var x` or `var x: T` (#3016).
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
                // §19.3 (v0.182, #1852): when the annotation is a bare TYPE
                // PARAMETER (a single-part Named ref with no user declaration),
                // resolve it through the active monomorphization substitution and
                // test against the concrete type argument — the same §19.3 rule as
                // the bare `case x of T` form (routed through the shared helper,
                // #1843's per-monomorphization descriptor for a constructed
                // generic). Without this the placeholder ref (null declaration,
                // non-primitive, so none of the concrete branches below matched)
                // fell to the `return nullptr` at the end → an unterminated
                // `case.end` (a malformed-IR ICE). This runs FIRST so a resolvable
                // T never reaches the concrete branches with the wrong (erased)
                // pattern name.
                if (rt->kind == sema::TypeRef::Kind::Named && !rt->declaration) {
                    const sema::TypeRef* ct = resolveTypeParamRef(rt);
                    if (ct && ct != rt) {
                        if (llvm::Value* m = emitTypeParamPatternTest(
                                ct, scrut, scrutTy, bp->type->span.begin))
                            return m;
                        // BELT-AND-SUSPENDERS (#1852 blocker 1): the param
                        // resolved to a concrete type the helper cannot lower —
                        // emit a TERMINATED never-match rather than fall through
                        // to the `return nullptr` below (malformed IR). No
                        // RESOLVED type-param target can ever produce malformed IR.
                        return builder_.getInt1(false);
                    } else if (emittingGenericTemplateBody_ && rt->astNode &&
                               rt->astNode->name &&
                               rt->astNode->name->parts.size() == 1 &&
                               templateGenericParamNames_.count(
                                   rt->astNode->name->parts.back())) {
                        // §19.3 (v0.182, #1852): unresolvable T in the dead
                        // template body — never-match keeps it IR-valid (see the
                        // bare `case x of T` sibling above).
                        return builder_.getInt1(false);
                    }
                }
                // #1854 / §19.3 + §31: a typed binding whose type is a BUILT-IN
                // exception (`var x: IOException`) — the built-ins carry no user
                // TypeSymbol so `rt->declaration` is null and the Named+declaration
                // block below misses them. Resolve the pattern type's descriptor
                // by name (builtinDesc_ via descriptorForPatternName) and emit the
                // runtime is-a test, exactly as the bare-name form does.
                if (rt->kind == sema::TypeRef::Kind::Named && !rt->declaration &&
                    isBuiltinExceptionTypeRef(rt)) {
                    // #1936 / §19.3: a VALUE-typed scrutinee (a non-pointer
                    // primitive/record/enum — e.g. `case 5 of var x:
                    // IOException`) can NEVER be a reference exception object, so
                    // the type-test is statically FALSE. Without this the
                    // pointer-only guard fell through to `return nullptr` below,
                    // leaving `case.end` unterminated → malformed IR (a hard
                    // codegen abort on a nonsensical program). This mirrors the
                    // bare-name NamedPattern path's `!isPointerTy() → false` gate
                    // (#1854, L5895). The typed-binding name slot is allocated in
                    // lowerCaseExpr independently of this result, so a
                    // compile-time false leaves the (statically dead) arm body
                    // well-formed — no unbound-slot pitfall (contrast #1949's
                    // destructuring branch, whose slots are bound inside the is-a
                    // block).
                    if (!scrut->getType()->isPointerTy())
                        return builder_.getInt1(false);
                    // The scrutinee is a heap exception object (object-like);
                    // emit the runtime is-a test.
                    if (llvm::GlobalVariable* desc =
                            descriptorForPatternName(bp->type->name.get()))
                        // #1938: string-in-Object guard (see emitStringGuardedIsA)
                        return emitStringGuardedIsA(scrut, desc, scrutTy);
                }
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
                    // #1936 / §19.3/§19.5: a CLASS or INTERFACE typed binding
                    // `var x: C` against a VALUE-typed (non-pointer) scrutinee can
                    // never match — a primitive/record/enum value is never a
                    // reference. The is-a paths below (emitStringGuardedIsA →
                    // emitIsA) would ICmp the non-pointer scrutinee against a null
                    // pointer and ABORT the backend (ICmpInst assertion / malformed
                    // IR). Sema rejects a CONCRETE value-scrutinee case up front
                    // (#1203/#1936), but a MONOMORPHIZED type-parameter scrutinee
                    // (`case t of var x: C` at T=int) is only decidable here, so a
                    // sema-only fix would still crash on it. Decide it statically
                    // false, mirroring the bare-name NamedPattern gate (its
                    // `!isPointerTy() → getInt1(false)` above) and the builtin-
                    // exception typed-binding gate just above.
                    if (!scrut->getType()->isPointerTy())
                        return builder_.getInt1(false);
                    // #1843 (§19.3 v0.182 / §19.6 v0.196): a CONSTRUCTED generic
                    // class/interface pattern (`var g: Tagged<int>`) tests against
                    // the per-monomorphization descriptor, so it matches only the
                    // named instantiation — a `Tagged<string>` value does not match
                    // the `Tagged<int>` arm.
                    // #1938: each class/interface is-a below guards a string-in-
                    // Object scrutinee (see emitStringGuardedIsA) — a `var x: C`
                    // typed binding shares the concrete-name fault the bare
                    // `case o of C` NamedPattern above had.
                    // §12.7/§2.8.3 rule 4/§19.3 (#2012, #2069): a `var x: Wrap<T>` binding
                    // whose T substitutes to a runtime-erased kind at this
                    // monomorphization collapses onto a sibling descriptor — REJECT
                    // (probes p_case_stmt/p_case_val: rc=0, answered TRUE on
                    // c3ea38c). This covers BOTH the statement- and value-form case
                    // (both route through patternMatches).
                    if (rejectCollapsedTypeTestTarget(rt, bp->type->span.begin))
                        return builder_.getInt1(false);
                    if (llvm::GlobalVariable* pid =
                            perInstDescriptorForTarget(rt))
                        return emitStringGuardedIsA(scrut, pid, scrutTy);
                    auto dit = descArray_.find(rt->declaration);
                    if (dit != descArray_.end())
                        return emitStringGuardedIsA(scrut, dit->second, scrutTy);
                    // #473: a named class with no descArray_ entry can still
                    // be resolved by name (mirrors the bare-NamedPattern path).
                    if (llvm::GlobalVariable* desc =
                            descriptorForPatternName(bp->type->name.get()))
                        return emitStringGuardedIsA(scrut, desc, scrutTy);
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
        // fix #2844 / §19.3 / §2.8.3 rule 4 (v0.240): an imported record's
        // TypeSymbol lives in `ctx_.importedSymbols()` — `ctx_.symbols()` is
        // local-only (SemaContext.hpp:90's `symbols_`), so a destructuring
        // pattern over an imported record (`case o of Pt(x, y) => …` with
        // `import Lib`) failed to resolve the head. With `recTy` null this
        // function returns `nullptr`, the case-pattern caller treats it as
        // "degraded to no-match" and never emits the binding alloca for the
        // arm body's `x`/`y` reads — the body lowers, but its reads of the
        // never-defined locals strand `case.end` with no terminator (the
        // IR-verification error the issue reproduces). Walk imported
        // symbols after the local pass, mirroring the established pattern
        // in LlvmBackend_Stmts.cpp:7657-7666 and LlvmBackend_Assign.cpp:
        // 10008-10020 (the same widening the #3197 fix applied for the
        // §29.2 enumerable case).
        //
        // fix #4050 (§19.3/§27.8, v0.294): a `type` alias of a record used
        // as a destructuring head (`AliasOfPoint(x, y)`) resolves to a
        // `sema::TypeAliasSymbol` — a distinct kind this lambda's
        // `dynamic_cast<TypeSymbol*>` alone does not recognize. This is a
        // SEPARATE gap from sema's OWN identical fix in
        // `ExprTC::bindNestedDestructuringNames`/the CaseExpr and CaseStmt
        // twins (ExprTypecheck.cpp / ExprTypecheck_Exprs.cpp): codegen does
        // NOT consume sema's resolved `recTy` here — it re-derives it
        // independently from the pattern's `ast::QualifiedName`, so the
        // sema-side fix alone left this crash reproducing (measured: fixing
        // only the sema arms did not clear the "Basic Block … does not have
        // terminator" failure for `case p of AliasOfPoint(x, y) => …`).
        // Resolved through `sema::resolveAliasToTypeSymbol`
        // (TypeResolution.hpp) — the SAME free function sema's arms call —
        // so this codegen site and every sema site share one implementation
        // rather than each re-deriving the alias-to-TypeSymbol lookup.
        auto considerSym = [&](const sema::Symbol* s) {
            if (auto* ts = dynamic_cast<const sema::TypeSymbol*>(s)) {
                if (ts->name == lastName &&
                    ts->kind == ast::TypeDeclKind::Record)
                    recTy = ts;
                return;
            }
            if (auto* aliasSym = dynamic_cast<const sema::TypeAliasSymbol*>(s)) {
                if (aliasSym->name != lastName) return;
                const sema::TypeSymbol* aliasedTs =
                    sema::resolveAliasToTypeSymbol(
                        ctx_, const_cast<sema::TypeAliasSymbol*>(aliasSym));
                if (aliasedTs && aliasedTs->kind == ast::TypeDeclKind::Record)
                    recTy = aliasedTs;
            }
        };
        for (const auto& sym : ctx_.symbols()) considerSym(sym.get());
        if (!recTy) {
            for (const sema::Symbol* sym : ctx_.importedSymbols())
                considerSym(sym);
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
            // #1949 / §19.3/§19.5: gate the is-a on the scrutinee being
            // OBJECT-LIKE, mirroring the bare-type-name NamedPattern path above
            // (see the `objectLike` gate before its emitStringGuardedIsA). A
            // record destructuring can only match a boxed record inside a
            // reference — an `Object`/interface (or a class instance, which is-a
            // then correctly rejects). A scrutinee whose STATIC type is a
            // vtable-less runtime reference like `string` reaches this branch as
            // a pointer (a `bz_string*`) too, but is never a record: with only
            // the #1938 `mayBeString` guard (false for a statically string-typed
            // scrutinee) it fell through to a plain emitIsA that walked the
            // string's `char* data` as a descriptor and FAULTED (SIGSEGV). A
            // string (or any non-object-like reference) can never be a record —
            // decide it statically. Cannot early-return getInt1(false) as the
            // NamedPattern path does: a destructuring arm body (`Pt(a,b) => a+b`)
            // references the bound names, which are registered as local slots by
            // the unbox block below; skipping it leaves `a`/`b` unresolved and
            // `case.end` without a terminator. Instead force the is-a to a
            // compile-time false so the unbox block is emitted (binding the
            // slots so the arm body still lowers) but is statically dead — the
            // faulting GEP/load is never reached at run time.
            bool objectLike = !scrutTy ||
                (scrutTy->kind == sema::TypeRef::Kind::Named &&
                 scrutTy->declaration &&
                 (scrutTy->declaration->kind == ast::TypeDeclKind::Class ||
                  scrutTy->declaration->kind == ast::TypeDeclKind::Interface)) ||
                (scrutTy->kind == sema::TypeRef::Kind::Primitive &&
                 scrutTy->primitive == sema::PrimitiveType::Object) ||
                isBuiltinExceptionTypeRef(scrutTy);
            // isA check — the guarded is-a leaves the builder at its join block,
            // like emitIsA leaves it at isa.end. #1938 / §19.3: an `Object`
            // holding a string would fault emitIsA walking the box descriptor;
            // guard it so `case o of Rec(...)` over a string cleanly no-matches.
            llvm::Value* isA = objectLike
                ? emitStringGuardedIsA(scrut, dit->second, scrutTy)
                : builder_.getInt1(false);
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
                    if (!disposeStack_.empty())
                        recordDeclScopeDepth(  // fix #4523
                            sp.nakedAnchor.get());
                    locals_[sp.nakedAnchor.get()] = slot;  // #1271 real Node anchor
                } else if (auto* vbp = dynamic_cast<const ast::BindingPattern*>(
                               sp.pattern.get())) {
                    llvm::AllocaInst* slot = entryAlloca(fv->getType());
                    builder_.CreateStore(fv, slot);
                    locals_[vbp] = slot;
                } else if (sp.pattern) {
                    // fix #3899 / §14.1 / §19.3: `recordMemberTypeResolved`
                    // (not the bare `recordMemberType`) so a generic-record
                    // field declared at a type parameter (`X: T` on
                    // `Box<int?>`) resolves to the concrete substituted type
                    // (`int?`) here, under `destrSubst` (installed above).
                    // With the unsubstituted type, `isNullableVal` below
                    // (keyed on this TypeRef) sees a non-nullable bare `T`
                    // for what is actually a `{i8,i32}` nullable-value
                    // struct, so a range/relational sub-pattern took the
                    // PLAIN dispatch on the boxed struct — malformed IR
                    // (`icmp {i8,i32}, i32`), an IR-verification failure.
                    const sema::TypeRef* fieldTy =
                        recordMemberTypeResolved(fields[i]);
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
        // #1977 / §19.3 / §9.4: value-type-PRIMITIVE scrutinee against a record
        // destructuring pattern (e.g. `case (n: int) of Pt(a, b) => …`) is never
        // a match — a primitive int is not a Pt record. The non-pointer branch
        // below unconditionally runs `CreateExtractValue(scrut, …)`, which aborts
        // the backend on a non-struct `scrut` (i32 / f64 / bool / inline-array /
        // enum primitive — anything LLVM treats as a scalar, not an aggregate):
        // the ExtractValue operand indices are invalid for the scalar type, and
        // the LLVM IR verifier asserts on the resulting malformed instruction.
        // Sibling of the #1949 fix's pointer-branch objectLike gate: there it
        // rejected a vtable-less reference scrutinee (a string, a runtime-
        // managed ref), here it rejects a value-type PRIMITIVE / enum / inline-
        // array scrutinee. A destructuring pattern over a non-record scrutinee
        // statically no-matches — emit the binding slots with `undef` placeholders
        // so the arm body (e.g. `a + b`) still lowers against the right LLVM
        // types (the arm is statically dead anyway: this branch returns false
        // and dispatch falls to `_`), and the slot bindings keep `case.end`
        // terminated. The pointer-branch fix (#1949) had to enter a separate
        // `unboxBB` to emit the bindings because the box-load was conditional;
        // here the binding emission is unconditional, so a single early-return
        // after stub-slot emission is the simpler shape. // per §19.3, §9.4
        if (!scrut->getType()->isStructTy()) {
            // Statically dead: emit each binding with an undef placeholder of the
            // field's recorded LLVM type (so the arm body's identifier reads
            // resolve to well-typed allocas), then no-match.
            for (std::size_t i = 0; i < dp->subPatterns.size(); ++i) {
                const auto& sp = dp->subPatterns[i];
                if (i >= fields.size()) break;
                // fix #3899 / §14.1: resolved (not bare) field type, same
                // generic-substitution reasoning as the live branch below —
                // this branch is statically dead but should still type the
                // undef placeholder as the CONCRETE (substituted) field type.
                const sema::TypeRef* fty = recordMemberTypeResolved(fields[i]);
                llvm::Type* llvmFty =
                    fty ? lowerTypeRef(const_cast<sema::TypeRef*>(fty))
                        : i32();  // fallback when fieldTy is unknown
                if (!sp.nakedBinding.empty()) {
                    llvm::AllocaInst* slot = entryAlloca(llvmFty);
                    builder_.CreateStore(
                        llvm::UndefValue::get(llvmFty), slot);
                    locals_[sp.nakedAnchor.get()] = slot;
                } else if (auto* vbp =
                               dynamic_cast<const ast::BindingPattern*>(
                                   sp.pattern.get())) {
                    llvm::AllocaInst* slot = entryAlloca(llvmFty);
                    builder_.CreateStore(
                        llvm::UndefValue::get(llvmFty), slot);
                    locals_[vbp] = slot;
                }
                // Non-binding sub-patterns are skipped — the arm body is
                // statically dead, so the sub-pattern check never runs.
            }
            return builder_.getInt1(false);
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
                //
                // fix #3899 / §14.1 / §19.3: `recordMemberTypeResolved`, not
                // the bare `recordMemberType` — see the identical reasoning
                // on the boxed-reference branch above. `destrSubst` (this
                // record scrutinee's RecordSubstGuard, constructed earlier in
                // this function) is active here, so the substitution is
                // available; using the unsubstituted declared type for a
                // generic record's bare-type-parameter field (`Box<int?>`'s
                // `X: T`) fed `isNullableVal` a non-nullable TypeRef for what
                // is actually a `{i8,i32}` nullable-value struct, so a
                // range/relational sub-pattern (`1..5`) took the PLAIN
                // dispatch directly on the boxed struct — an IR-verification
                // failure (`icmp {i8,i32}, i32`), never reaching the
                // nv.range.match / nv.relop.match handlers above.
                const sema::TypeRef* fieldTy =
                    recordMemberTypeResolved(fields[i]);
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
// DestructuringPattern branch does (by name, falling back to
// `ctx_.importedSymbols()` after the local pass; see #2844 / §2.8.3 rule 4).
// There is no sema-attached back-pointer on the AST node to reuse.
void Emitter::bindLambdaDestructureParam(const ast::DestructuringPattern& dp,
                                         llvm::Value* structVal) {
    if (!dp.typeName || !structVal) return;
    std::string_view lastName = dp.typeName->parts.empty()
        ? std::string_view{} : dp.typeName->parts.back();
    const sema::TypeSymbol* recTy = nullptr;
    // fix #2844 / §27.4 / §2.8.3 rule 4 (v0.240): see the matching note
    // in patternMatches' DestructuringPattern branch above — imported
    // records must be considered, or the lookup returns null and the
    // binding silently degrades (the arm body's `x` reads strand the
    // basic block without a terminator, exactly as the case-pattern
    // shape). The sema bindLambdaDestructure site at
    // ExprTypecheck_Exprs.cpp:395 was the same defect on the
    // typecheck side; both widen in this commit.
    auto considerSym = [&](const sema::Symbol* s) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(s);
        if (!ts || ts->name != lastName ||
            ts->kind != ast::TypeDeclKind::Record) return;
        recTy = ts;
    };
    for (const auto& sym : ctx_.symbols()) considerSym(sym.get());
    if (!recTy) {
        for (const sema::Symbol* sym : ctx_.importedSymbols())
            considerSym(sym);
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
    // fix #5137 (§7.11, v0.313): the §7.11 'no three-valued logic' gate at
    // if-expression-conditions (src/sema/ExprTypecheck.cpp ~6201, the
    // "if-expression" ctx) admits a `bool?` condition narrowed non-null by §7.4.
    // Sema returns success and codegen receives the condition as the declared
    // `{i8 hasVal; i1 payload}` struct, but `CreateCondBr` below needs an
    // `i1` — extract the payload here. The pre-extraction soundness
    // precondition of unwrapNarrowedNullableOperand (a sema gate that rejects
    // the un-narrowed form) is satisfied by the #5137 sema half in this same
    // lane.
    cond = unwrapNarrowedNullableOperand(cond, c.condition.get());

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

    // fix #1948 / §20 / §7.7 / §28.3 (producers arm-meet, v0.184; v0.220) /
    // §28.9: an if-expression `(if c then v1 else v2)` whose result is a
    // (non-nullable) owned `string`. §28.3's producers table makes the result
    // the arm-meet of the branches "identically to the `a ?? b` row" (v0.184):
    // "owned ⊓ owned = owned — exactly one arm's value is produced at runtime
    // and is discharged once". For a runtime-managed `string` (§28.9) that
    // means the result must carry EXACTLY +1, on WHICHEVER branch is taken.
    // Because the PHI joins the branches into one value, the consumer (return /
    // assign / bind) makes ONE retain-or-transfer decision for the merged
    // value, so each branch is normalized to +1 HERE, in its own block, before
    // the join — mirroring the per-arm normalization lowerCaseExpr applies to
    // the sibling case/when result (#1934) and the ReturnStmt / Assign paths
    // apply to a plain `return <concat>`. A branch that is itself a +1 producer
    // (`+`/interpolation/nested string-returning call, or a string local owning
    // +1) already carries its single reference — leave it; any other branch (a
    // param/field borrow, a string-literal sentinel) is a +0 borrow and is
    // RETAINED to mint the +1 (bz_string_retain is a no-op on a
    // sentinel/literal/null). The whole if-expression is then a single +1
    // producer (isPlusOneStringProducer recognizes it), so the consumer
    // TRANSFERS the reference out without a second retain. Both halves are
    // required: recognition WITHOUT this per-branch mint would drop a borrow
    // branch's reference (a use-after-free); the mint WITHOUT recognition would
    // still let the consumer double-retain a producer branch (the leak — #1948).
    //
    // fix #1951 (conditional escape): RETAIN a bare read of a named string LOCAL
    // owning +1 written as a BRANCH — do NOT transfer it (the removed
    // `!returnedStringLocalOwnsPlusOne` guard). An if-branch `(if c then s else
    // …)` selects `s` on ONE path only; transferring its single reference out
    // leaked `s` on the other path, where the suppressed scope-end release never
    // ran. Retaining mints a fresh +1 for the branch result and leaves `s`'s own
    // +1 in its slot, coordinated with the StringEscapeWalker relaxation (a
    // string-typed if-branch read is a borrow → the local stays release
    // eligible). The ReturnStmt/arrow/lambda paths keep the transfer for an
    // UNCONDITIONAL escape (`return s`). See lowerCaseExpr for the full rationale.
    //
    // NULLABLE-INCLUSIVE (fix #2200, nullable analog of #1951): the same
    // reasoning as lowerCaseExpr above — after #2196 the whole `string?`
    // value path is refcounted, so the pre-#2196 unsoundness ("minting +1
    // for a `string?` if-expression leaks because the nullable return path
    // is un-refcounted") is gone. Extending the per-branch mint + the
    // StringEscapeWalker relaxation (coordinated — see
    // isStringResultArmMeet) closes the leak a `string?` conditional-escape
    // caused. bz_string_retain is null-safe, so a `null` branch is a no-op.
    // // per #1951, #2200, §28.3, §28.9
    if (resultSemaType &&
        resultSemaType->kind == sema::TypeRef::Kind::Primitive &&
        resultSemaType->primitive == sema::PrimitiveType::String) {
        if (tv && !thenDiv && tv->getType()->isPointerTy() &&
            !isPlusOneStringProducer(c.thenValue.get())) {
            builder_.SetInsertPoint(thenEnd);
            emitStringRetain(tv);
        }
        if (ev && !elseDiv && ev->getType()->isPointerTy() &&
            !isPlusOneStringProducer(c.elseValue.get())) {
            builder_.SetInsertPoint(elseEnd);
            emitStringRetain(ev);
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
