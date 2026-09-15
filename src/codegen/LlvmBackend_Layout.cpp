#include "codegen/LlvmBackend_impl.hpp"
#include "sema/GenericParams.hpp"  // fix #3143: the ONE both-sources generic-param walk (§14.1/§12.9/§2.5)

namespace bezel::codegen {

llvm::Type* Emitter::lowerPrimitive(sema::PrimitiveType p) {
    using P = sema::PrimitiveType;
    switch (p) {
        case P::Bool:   return i1();
        case P::Float:  return llvm::Type::getFloatTy(llvm_);
        case P::Double: return llvm::Type::getDoubleTy(llvm_);
        // CG25: integer widths (§5). Char is a 32-bit code point.
        case P::Byte: case P::SByte:   return llvm::Type::getInt8Ty(llvm_);
        case P::Short: case P::UShort: return llvm::Type::getInt16Ty(llvm_);
        case P::Long: case P::ULong:   return i64();
        case P::Int: case P::UInt: case P::Char:
                                       return i32();
        // §4.4 fix #521: decimal is a 128-bit value type { i32 lo, i32 mid, i32 hi, i32 flags }.
        // Layout is .NET-compatible: 96-bit mantissa (lo/mid/hi) + scale + sign in flags.
        case P::Decimal:               return decimalStructType();
        // CG27a (§4.6): string is a reference type — pointer to bz_string.
        case P::String:                return ptrTy();
        // §8.1: Object is the universal reference root — every reference type
        // reaches it via its heap representation, so an Object-typed value is a
        // heap pointer (like string above), not a scalar. Lowering it to a
        // pointer is what lets a type test (§12.7/§17.4) on an Object/Object?
        // operand load its vtable and compare descriptors; without this the
        // operand was an i32 and the type test's ICmp against a null pointer
        // aborted the compiler (#399).
        case P::Object:                return ptrTy();
        default:                       return i32();
    }
}


bool Emitter::isUnsignedPrim(sema::PrimitiveType p) {
    using P = sema::PrimitiveType;
    return p == P::Byte || p == P::UShort || p == P::UInt ||
           p == P::ULong || p == P::Char;
}


// #814 / §6.2 / §5.1: the signedness of a binary integer OPERATION (the
// comparison predicate, the overflow/div/rem intrinsic) is the signedness of the
// COMMON (promoted) type, not "either operand is unsigned". §6.2 promotes the
// narrower operand to the larger type; the common type is unsigned ONLY when
// BOTH operands are unsigned — if either is signed, the unsigned operand widens
// to a signed type (byte→int, uint→long per the §5.1 table; `ulong + long` has
// no common integer type and is a §6.2 compile error), so the operation is
// signed. (Earlier this OR'd the operands, so `byte < int(-3)` used ICmpULT and
// `byte + int(-3)` used uadd_with_overflow — treating the negative value as a
// huge unsigned → wrong boolean / spurious OverflowException.)
//
// NOTE: per-operand WIDENING (zext vs sext of the narrower operand) and the
// fill of a right-SHIFT use a *different* signedness — each operand's own (and,
// for shift, the LEFT operand's). Those sites use isUnsignedTargetExpr directly.
bool Emitter::isUnsignedBinOperand(const ast::BinaryExpr& bin) {
    // per §8.4/§10.3 (#1189): typeRefIsUnsigned also recognises a Named-kind
    // enum operand (its signedness is its underlying primitive's), so a
    // relational comparison of two unsigned-backed enums selects icmp ult.
    auto unsignedExpr = [&](const ast::Expr* e) {
        if (!e) return false;
        // §11.8 (v0.176, #807): a T-typed operand under a constraint-operator
        // dispatch — resolve the type parameter to its concrete substitution
        // so a `T = uint/ulong` specialization selects the unsigned ops.
        return typeRefIsUnsigned(resolveTypeParamRef(ctx_.exprType(*e)));
    };
    return unsignedExpr(bin.left.get()) && unsignedExpr(bin.right.get());
}


bool Emitter::isUnsignedTargetExpr(const ast::Expr& e) {
    // per §8.4/§10.3 (#1189): include a Named-kind enum target, whose signedness
    // is that of its underlying primitive (byte/ushort/uint/ulong → unsigned).
    // §11.8 (v0.176, #807): resolve a T-typed operand through the active
    // substitution first (constraint-operator dispatch on T = uint/ulong).
    return typeRefIsUnsigned(resolveTypeParamRef(ctx_.exprType(e)));
}


bool Emitter::isCharTargetExpr(const ast::Expr& e) {
    // §6.6/§5.2 (#1961): the compound/increment write-back applies the §5.2
    // code-point narrowing only for a `char` target. Resolve a T-typed target
    // through the active substitution first (a generic body specialized on
    // T = char), mirroring isUnsignedTargetExpr.
    //
    // §7.11 (#2888): `char?` is included. The pre-#2888 `!rt->nullable` clause
    // was correct only while a `char?` LHS never reached emitCompoundOp — the
    // open-coded #1964 lifting in lowerStmt computed its own `isCharU` from the
    // target TypeRef and passed it down. emitCompoundOp now owns the lifting,
    // unwraps the `{i8, i32}` payload BEFORE its char arm and passes this flag
    // through, so excluding `char?` here would drop §5.2's code-point check on
    // the both-present branch — a silently invalid `char`, which is the
    // type-invariant violation #1961 exists to prevent. This predicate's ONLY
    // consumers are emitCompoundOp's `charTarget` argument.
    const sema::TypeRef* rt = resolveTypeParamRef(ctx_.exprType(e));
    return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
           rt->primitive == sema::PrimitiveType::Char;
}


llvm::Value* Emitter::lvalueAddr(const ast::Expr& target, llvm::Type*& elemTy) {
    // fix #3090 (§12.7 / §8.6, v0.252): a §12.7 value-narrowed identifier
    // denotes the boxed VALUE, so its address is the box payload — not the
    // `Object` slot, whose bytes are the box POINTER. Checked FIRST, ahead of
    // the ref-param and local-slot arms, because those are exactly the arms
    // that would otherwise hand back the reference slot: `o[0]` inside
    // `if o is [3]int then … end` reached the inline-array arm below with
    // `elemTy == ptr`, failed its `dyn_cast<ArrayType>`, and the whole read
    // dropped (#3079's "argument failed to lower" note, or #2434's
    // zero-initialized slot). Declines — returning nullptr — for every read
    // that is not one, so no other lvalue changes.
    //
    // A direct reassignment `o := …` cannot reach this arm: §12.7's
    // "Reassignment within the controlled block invalidates the narrowing" is
    // implemented in sema by erasing the narrowing BEFORE the assignment
    // target is visited (ExprTypecheck.cpp's AssignmentStmt arm), so the
    // target identifier carries its declared `Object` type and the predicate
    // declines. A `ref`/`out` argument is likewise erased (#1188).
    //
    // A write THROUGH the narrowed binding (`o[0] := 9`, whose lvalue chain is
    // rooted here) does reach this arm, and lands in the box payload — coherent
    // (readable back through the same binding) but not something §12.7 or §8.6
    // decides; §23.5 rejects the structurally identical indexer-result shape.
    // Recorded as a spec question rather than settled in code: **#3117**.
    //
    // ⚠ NOTE FOR THE NEXT READER: this arm EMITS IR (one `load` of the slot and
    // one `GEP`) before the pure-lookup arms below, so `lvalueAddr` is no longer
    // side-effect-free to ask "is this an lvalue?" of. It is harmless — the load
    // reads the slot the un-narrowed read would have read anyway and never
    // dereferences the box, so a caller that discards the answer leaves dead but
    // valid instructions, and LLVM CSEs duplicates from repeated queries on the
    // same operand. It fires only for a §12.7 value-narrowed identifier; every
    // other target returns nullptr before any instruction is built.
    if (llvm::Value* p = narrowedBoxPayloadAddr(target, elemTy)) return p;
    // CG50: a ref/out parameter — its address is the incoming pointer.
    if (llvm::Value* a = refParamAddr(target, elemTy)) return a;
    if (llvm::AllocaInst* slot = localSlot(target)) {
        // §27.5: a captured ("boxed") local's address is its shared cell.
        return boxedStorage(slot, elemTy);
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target)) {
        if (llvm::Value* a = fieldAddr(*ma, elemTy)) return a;
    }
    if (llvm::GlobalVariable* gv = staticFieldGlobal(target)) {
        // #720 / §22.13 / #1513: a static-field lvalue (write or compound
        // assign) is a "first use" — lazily initialize the owning type (or,
        // for a generic instantiation's per-instantiation global, the SHARED
        // cross-module instantiation state, §2.8.3 rule 4) BEFORE the access,
        // so a write-before-read is not later clobbered by the lazy initializer.
        if (auto giIt = genericInstGlobalKeyOf_.find(gv);
            giIt != genericInstGlobalKeyOf_.end()) {
            emitEnsureGenericInstStaticInit(giIt->second);
        } else if (auto* sym = ctx_.exprSymbol(target)) {
            emitEnsureStaticInit(memberOwner(sym));
        }
        elemTy = gv->getValueType();
        return gv;
    }
    // §15.1: inline array element lvalue `a[i]`.
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&target)) {
        if (ix->target && !ix->indices.empty() && ix->indices[0].value) {
            llvm::Type* arrSlotTy = nullptr;
            llvm::Value* arrAddr = lvalueAddr(*ix->target, arrSlotTy);
            if (arrAddr && arrSlotTy) {
                if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(arrSlotTy)) {
                    llvm::Value* idx = lowerExpr(*ix->indices[0].value);
                    if (idx) {
                        // Bounds check (§15.1): IndexOutOfRangeException.
                        uint64_t sz = arrTy->getNumElements();
                        llvm::Value* idx64 = idx->getType()->isIntegerTy(64)
                            ? idx : builder_.CreateSExt(idx, i64());
                        llvm::Value* oor = builder_.CreateICmpUGE(
                            idx64, builder_.getInt64(sz), "arr.wr.oor");
                        emitGuardThrow(oor, "IndexOutOfRangeException");
                        elemTy = arrTy->getElementType();
                        return builder_.CreateGEP(
                            arrTy, arrAddr, {builder_.getInt64(0), idx});
                    }
                }
            }
        }
    }
    return implicitFieldAddr(target, elemTy);
}


llvm::Value* Emitter::emitImplicitOne(llvm::Type* t) {
    if (!t) return nullptr;
    if (t->isFloatingPointTy()) return llvm::ConstantFP::get(t, 1.0);
    // §6.4: `decimal` is a STRUCT-typed primitive — `ConstantInt::get` does not
    // build a constant of that type at all.
    if (t == decimalStructType()) return emitDecimalOne();
    if (t->isIntegerTy()) return llvm::ConstantInt::get(t, 1);
    // §7.11 (#2888): for a nullable value LHS the implicit `1` is the BARE
    // payload `1`, not a wrapped `T?`. §7.11, verbatim: "A mixed expression in
    // which one operand is `T?` and the other is the non-nullable `T` lifts as
    // well — the non-nullable operand is treated as present (never `null`)" —
    // which is exactly how emitCompoundOp's lifting arm reads a non-tagged
    // right operand. (`n++` on a `null` `int?` therefore stays `null`, per
    // §17.6's identification of `x++` with `x := x + 1`.)
    if (llvm::Type* pay = nullableValuePayloadTy(t)) return emitImplicitOne(pay);
    return nullptr;
}


// fix #2904 / §25.1 / §25.2 / §6.6: see the declaration comment in
// LlvmBackend_impl.hpp. §25.2 "Resolution", verbatim: "For `a <op> b` on
// non-primitive operands, the candidate set is the union of the `operator <op>`
// overloads declared on the **static types of both operands, including
// declarations inherited from their base classes** … Parameters match operands
// **in order** — there is no automatic commutation". So the target's type is
// searched first (the common LHS-declared case) and then the value's (#2156's
// RHS-declared shape, e.g. `operator *(int, Scale)` living on `Scale`), each
// through its own base chain.
sema::MethodSymbol* Emitter::findCompoundOperatorOverload(
        const std::string& binOp, const sema::TypeRef* leftTy,
        const sema::TypeRef* rightTy) {
    if (binOp.empty()) return nullptr;
    // fix #2932 (§25.2, v0.240): the CLASS base chain of a type symbol, walked
    // by `classBaseOf`. `baseTypes` carries interfaces too; only a CLASS base
    // participates here (§11.8's static interface members are the
    // type-parameter path, resolved at instantiation, not this one).
    auto classBaseOf = [](const sema::TypeSymbol* t) -> const sema::TypeSymbol* {
        if (!t) return nullptr;
        for (const sema::TypeSymbol* b : t->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) return b;
        return nullptr;
    };
    // §25.2: match the operator's parameter types against the operand types IN
    // ORDER (left→param0, right→param1). Sema has already validated that the
    // compound is well-typed, so this only DISAMBIGUATES when one type declares
    // several overloads of the same token. A Primitive parameter matches a
    // Primitive operand by kind (numeric widening is downstream, as for the
    // binary form).
    //
    // fix #2932 (§25.2, v0.240): a Named parameter matches a Named operand by
    // declaration OR by the operand's CLASS BASE CHAIN — §25.2, verbatim: "A
    // derived-typed operand satisfies a base-typed parameter by the ordinary
    // argument rules." The exact-declaration test this had was half of why
    // `n += d` with `operator +(int, Base)` and `d: Derived` aborted the
    // compile: even reaching the declaration on `Base`, `Derived` did not match
    // its `Base`-typed parameter. (Interface conformance is deliberately NOT
    // admitted here: the measured defect is the class chain, and widening to
    // interfaces would newly SELECT operators for shapes nothing has measured.)
    auto paramMatches = [&](sema::Symbol* paramSym,
                            const sema::TypeRef* operandTy) -> bool {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(paramSym);
        if (!ps) return false;
        sema::TypeRef* pt = ps->declType ? ctx_.resolvedType(*ps->declType)
                                         : ps->resolvedDeclType;
        if (!pt || !operandTy) return false;
        if (pt->kind == sema::TypeRef::Kind::Named) {
            if (operandTy->kind != sema::TypeRef::Kind::Named ||
                !pt->declaration || !operandTy->declaration)
                return false;
            std::unordered_set<const sema::TypeSymbol*> seen;
            for (const sema::TypeSymbol* t = operandTy->declaration;
                 t && seen.insert(t).second; t = classBaseOf(t))
                if (t == pt->declaration) return true;
            return false;
        }
        if (pt->kind == sema::TypeRef::Kind::Primitive)
            return operandTy->kind == sema::TypeRef::Kind::Primitive;
        return false;
    };
    // fix #2932 (§25.2, v0.240): "including declarations inherited from their
    // base classes (a derived-typed operand reaches its base's operators …)".
    // The scan this function was extracted from (`#610`/`#2156`) looked at a
    // type's OWN members only, so a DERIVED-typed operand contributed no
    // candidates at all: measured on 20cc955, `n += d` with
    // `operator +(int, Base)` declared on `d`'s base ABORTED the compile
    // ("Call parameter type does not match function signature! … call
    // { i32, i1 } @llvm.sadd.with.overflow.i32(i32 %56, ptr %55)") while the
    // plain binary `n + d` one line above was correct — `lowerBinary` walks the
    // chain and this did not. §12.2's no-hiding rule "prevents a derived class
    // from redeclaring an operator token its base declares, so one chain
    // contributes no duplicate candidates" — so the FIRST match up the chain is
    // THE match and there is no ambiguity to resolve. The visited set guards a
    // malformed cyclic chain.
    auto findOnType = [&](const sema::TypeSymbol* ts) -> sema::MethodSymbol* {
        std::unordered_set<const sema::TypeSymbol*> seen;
        for (const sema::TypeSymbol* t = ts; t && seen.insert(t).second;
             t = classBaseOf(t)) {
            for (sema::Symbol* m : t->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (ms && ms->isOperator && ms->operatorToken == binOp &&
                    ms->params.size() == 2 &&
                    paramMatches(ms->params[0], leftTy) &&
                    paramMatches(ms->params[1], rightTy))
                    return ms;
            }
        }
        return nullptr;
    };
    if (leftTy && leftTy->kind == sema::TypeRef::Kind::Named &&
        leftTy->declaration)
        if (sema::MethodSymbol* m = findOnType(leftTy->declaration)) return m;
    if (rightTy && rightTy->kind == sema::TypeRef::Kind::Named &&
        rightTy->declaration)
        return findOnType(rightTy->declaration);
    return nullptr;
}


// fix #2904 / §25.1 / §25.2 / §6.6: `emitCompoundOp` with the §25 overload arm
// in front of it — see the declaration comment in LlvmBackend_impl.hpp. The
// lookup must run BEFORE emitCompoundOp, not after it: emitCompoundOp's integer
// tail hands a record struct to `llvm.sadd.with.overflow`, which the IR verifier
// rejects, so the compile ABORTS rather than falling through to anything.
llvm::Value* Emitter::emitCompoundOpTyped(const std::string& op, llvm::Value* l,
                                          llvm::Value* r,
                                          const sema::TypeRef* leftTy,
                                          const sema::TypeRef* rightTy, bool fp,
                                          bool uns, bool charTarget,
                                          bool rUns) {
    // fix #3924 (§6.6/§7.11/§17.6, v0.288): every accessor-mediated compound
    // assignment (block-form property, virtual/interface property, imported
    // indexer) funnels through this ONE function, but each caller lowers `r`
    // straight from its own RHS expression with no unwrap — so #3830's sema
    // change (reading a compound assignment's §7.4-NARROWED operand type)
    // reaches here too: `b.NP += m` for a narrowed-nullable local `m` handed
    // the still-tagged `{i8, T}` struct down to `emitCompoundOp`'s integer/
    // decimal tails exactly as the local/field/static call site in
    // LlvmBackend_Stmts.cpp did before its own #3924 fix (measured: a
    // block-form `int` property IR-verifier-aborted identically — "Call
    // parameter type does not match function signature!"). `rightTy` is
    // already the RHS's declared (un-narrowed) sema type every caller
    // threads in for the §25.2 overload lookup just below, so
    // `unwrapNarrowedNullableOperand`'s TypeRef overload needs no new
    // plumbing. Placed before the overload lookup (harmless there: a Named
    // type with a real `operator <op>` overload is never `isNullablePrimTy`,
    // so the gate never fires for it) so BOTH arms of this function see the
    // unwrapped operand.
    r = unwrapNarrowedNullableOperand(r, rightTy);
    if (sema::MethodSymbol* om =
            findCompoundOperatorOverload(op, leftTy, rightTy)) {
        // §25.1 / §25.2: dispatch to the user-defined binary operator with the
        // arguments in SOURCE order {target, value} — the §25.2 in-order match.
        // A failed call DECLINES (returns null) so the caller reports its own
        // diagnostic; falling through to emitCompoundOp would abort the compile
        // on malformed IR, which is the very defect this arm exists to remove.
        std::vector<llvm::Value*> args{l, r};
        return emitMethodCall(om, args, /*baseQualified=*/false);
    }
    return emitCompoundOp(op, l, r, fp, uns, charTarget, rUns);
}


llvm::Value* Emitter::emitCompoundOp(const std::string& op, llvm::Value* l,
                            llvm::Value* r, bool fp, bool uns, bool charTarget,
                            bool rUns) {
    // §7.11 (#2888): a nullable VALUE left operand. It must be tested FIRST —
    // ahead of the char arm, which would otherwise hand the whole tagged struct
    // to emitCharRangeCheck, and ahead of the integer tail, which handed it to
    // `llvm.sadd.with.overflow` (an IR-verifier abort:
    // "Call parameter type does not match function signature! … { i8, i32 }").
    //
    // §7.11, verbatim (the document's inline section links rendered as bare
    // `§N`): "A lifted binary operator produces `null` if *either* operand is
    // `null`", and "The runtime checks each operator already performs —
    // `OverflowException` on integer overflow (§6.1), `DivideByZeroException`
    // on integer or `decimal` division/`mod` by zero (§6.1) — apply only when
    // both operands are present (non-null); when the lifted result is `null` by
    // reason of a null operand, no underlying operation runs and no check
    // fires." That is a CONTROL-FLOW construct: a
    // hasValue-guarded branch whose present arm runs the ordinary checked
    // operation on the unwrapped payloads and rewraps, joined by a PHI with a
    // `null` incoming from the guard.
    //
    // THE ARM LIVES HERE, in the one function every compound/increment lowering
    // funnels through, rather than at the callers (#2888). It was open-coded in
    // exactly ONE of them — lowerStmt's `#1964` AssignmentStmt block — so every
    // ACCESSOR-mediated and COLLECTION spelling was a live defect: `b.NP += 3`
    // (lowerPropertyCompound), `b[0] += 4` (lowerIndexerCompoundCore),
    // `iface.P += 5.0` (the #481 arm of lowerStmt), the imported-property path
    // (lowerImportedPropertyCompoundCore) and `arr[0] += 3` / `dict[k] += 4`
    // (lowerCollectionIndexerCompound) all aborted the compile on malformed IR.
    // Putting it in the callers would have made SIX copies of one rule; #2883
    // had just made the same call for the `decimal` arm below.
    //
    // §7.11's "A mixed expression in which one operand is `T?` and the other is
    // the non-nullable `T` lifts as well — the non-nullable operand is treated
    // as present (never `null`)": a right operand that is not itself tagged
    // contributes a constant-true tag. For a `decimal?` LHS "is the RHS tagged"
    // cannot be "is it a struct", because the bare `decimal` payload is ALSO a
    // struct — it is "a struct that is not the bare `%bz.decimal`" (#2149),
    // which covers both a `decimal?` RHS and a nullable-INT RHS.
    if (llvm::Type* payTy = nullableValuePayloadTy(l->getType())) {
        llvm::Type* nty = l->getType();
        const bool decimalPayload = (payTy == decimalStructType());
        llvm::Value* lTag = nullablePrimHasValue(l);   // i1
        llvm::Value* lPay = nullablePrimValue(l);      // payTy
        const bool rNullable =
            decimalPayload ? (r->getType()->isStructTy() &&
                              r->getType() != decimalStructType())
                           : r->getType()->isStructTy();
        llvm::Value* rTag = rNullable ? nullablePrimHasValue(r)
                                      : builder_.getTrue();
        llvm::Value* rPay = rNullable ? nullablePrimValue(r) : r;
        llvm::Value* both = builder_.CreateAnd(lTag, rTag, "ncmp.both");
        auto* entryBB = builder_.GetInsertBlock();
        auto* opBB = llvm::BasicBlock::Create(llvm_, "ncmp.op", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "ncmp.end", currentFn_);
        builder_.CreateCondBr(both, opBB, endBB);
        // --- opBB: BOTH present → the ordinary checked operation on payloads.
        builder_.SetInsertPoint(opBB);
        llvm::Value* pay = nullptr;
        if (decimalPayload) {
            // §6.4/§5.1 (#2141): an integer RHS (`d += 5`, or the unwrapped
            // payload of an `int?` RHS — #2149) §5.1-converts to `decimal`; a
            // decimal RHS is used as-is. §6.2 admits no decimal/binary-float
            // mixing, so sema has already rejected anything else.
            rPay = coerceNumericToType(rPay, decimalStructType(), rUns);
            if (rPay->getType() == decimalStructType())
                pay = emitDecimalArith(op, lPay, rPay);
        } else {
            // §5.1 widening of the RHS payload to the LHS payload width
            // (int→i64, int→double, float→double). Integer same-width and
            // narrowing cases are handled by the recursion itself, exactly as
            // on the non-nullable path.
            rPay = coerceNumericToType(rPay, payTy, rUns);
            pay = emitCompoundOp(op, lPay, rPay, payTy->isFloatingPointTy(),
                                 uns, charTarget, rUns);
        }
        // The payload op may have spun off guard-throw blocks (§6.1 overflow,
        // §5.2 char range, §6.1 divide-by-zero), so the PHI's predecessor is
        // the CURRENT block, not opBB.
        llvm::Value* wrapped = nullptr;
        if (pay) {
            wrapped = builder_.CreateInsertValue(
                llvm::UndefValue::get(nty),
                llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
            wrapped = builder_.CreateInsertValue(wrapped, pay, {1u});
        }
        auto* opEndBB = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        // A payload the underlying op cannot lower DECLINES (the caller reports
        // its own "could not lower the RHS/operator" diagnostic). Returning a
        // PHI whose present arm is `null` would be a SILENT wrong value — the
        // program would store `null` where §7.11 defines a value.
        if (!pay) return nullptr;
        llvm::PHINode* res = builder_.CreatePHI(nty, 2, "ncmp.res");
        res->addIncoming(wrapped, opEndBB);
        // Null-by-null-operand: the lifted result is `null`, and no underlying
        // operation ran on this edge.
        res->addIncoming(llvm::ConstantAggregateZero::get(nty), entryBB);
        return res;
    }
    // §6.6/§5.2 (#1961): a `char` LHS compound assignment/increment desugars to
    // `c := char(c OP e)` — the arithmetic runs at int width with the §6.1/§6.2
    // checked posture (as for any integer), and the result is narrowed back to
    // char by the §5.2 code-point check: a value outside U+0000–U+10FFFF or in
    // the surrogate range U+D800–U+DFFF raises OverflowException rather than
    // storing an invalid char. `char` lowers to i32 exactly like int/uint, so
    // emitCompoundOp cannot see the char-ness from the operands — the caller
    // flags it. Recurse for the arithmetic (charTarget=false) and route the
    // result through the same §5.2 helper the explicit `char(n)` cast uses, so
    // the write-back and the cast enforce the identical contract. Before this
    // fix the compound/increment path skipped the check and produced an invalid
    // char (a type-invariant violation).
    //
    // §6.2 (#1963): the arithmetic itself runs SIGNED. `char` implicitly
    // converts to `int` (§5.1), a signed type, so in `c OP e` the char promotes
    // to `int` and the operation is signed — exactly what the explicit
    // `char(c OP e)` cast computes (an `int` add/sub/mul, then the range check).
    // The caller's `uns` is `isUnsignedPrim(char) == true` (char is unsigned for
    // the sub-i32 widening / right-shift / cast paths), but for the char
    // compound ARITHMETIC it must NOT select the unsigned overflow intrinsic:
    // `char(5) += -1` under `uadd.with.overflow(5, 0xFFFFFFFF)` reports overflow
    // and raises, when §6.6/§6.2 give `char(5 + (-1)) = char(4)`. Force signed
    // arithmetic; the §5.2 code-point range is 0–U+10FFFF, well within `int`'s
    // range, so signed i32 arithmetic never spuriously overflows before the
    // §5.2 range check (which still fires for a genuinely out-of-range result).
    if (charTarget) {
        // §6.6/§5.2 (#1996): the arithmetic must run at the WIDER of char's i32
        // and the RHS width so a RHS that exceeds i32 range reaches the §5.2
        // range check UNTRUNCATED. `char` lowers to i32; the recursion below
        // otherwise coerces the RHS to i32 FIRST (the #1042 width-match at the
        // end of this function), so `char(0) += 2^32` (a `long` RHS) truncated
        // 2^32 → 0 and stored char(0) with no raise — while the explicit
        // `char(c + 2^32L)` cast correctly raises. Widen the char operand to the
        // RHS width (`char` is non-negative in 0..U+10FFFF, so sign/zero extend
        // agree) so the op runs at the full width; emitCharRangeCheck then sees
        // the true i64 result (its SExt is a no-op at that width) and truncates
        // back to i32 only after the range check passes. Matches the explicit-
        // cast form exactly (the #1996 compound-vs-cast parity gap).
        llvm::Value* lw = l;
        if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy() &&
            r->getType()->getIntegerBitWidth() >
                l->getType()->getIntegerBitWidth())
            lw = builder_.CreateSExt(l, r->getType(), "char.wide");
        llvm::Value* res = emitCompoundOp(op, lw, r, /*fp=*/false, /*uns=*/false);
        if (!res) return nullptr;
        return emitCharRangeCheck(res);
    }
    // §6.4/§5.1 (#2883): `decimal` is a STRUCT-typed primitive (`%bz.decimal`),
    // not a scalar — every arm below assumes a scalar, and the integer tail
    // hands the struct straight to `llvm.sadd.with.overflow`, which the IR
    // verifier rejects ("Intrinsic has incorrect return type!") so the compile
    // ABORTS on a legal program. Route it to the §6.4 decimal helper, which
    // carries §6.4's "raises `OverflowException` on overflow and
    // `DivideByZeroException` on division by zero" — exactly what the LOCAL
    // `d += x` / `d++` statement paths already do (LlvmBackend_Stmts.cpp).
    //
    // The arm lives HERE rather than at each caller because the hole was one
    // missing arm reached from FOUR call sites, each a separate live ICE:
    // `d.P += 1m` on a block-form property (lowerPropertyCompound),
    // `d[0] += 1m` / `d[0]++` on an indexer (lowerIndexerCompoundCore, whose
    // own `lowerRhs` already synthesized `emitDecimalOne()` one line earlier),
    // `i.P += 1m` through a virtual/interface property (the #481 arm of
    // lowerStmt), and the imported-property path #2862 had to open-code a
    // caller-side check to dodge — that check is deleted with this fix.
    //
    // §5.1/§6.2: an integer RHS widens to `decimal` (`d.P += 5`). The local
    // path pre-coerces the RHS to the slot type before it reaches this function
    // (`coerceNumericToType(rhs, elemTy, …)`), but the property/indexer callers
    // pass the RHS at its own natural width, so the widening is applied here —
    // without it `emitDecimalArith` would store an i32 into a `%bz.decimal`
    // slot. If the RHS is still not a decimal afterwards, DECLINE (a diagnostic
    // beats malformed IR); §6.2 admits no decimal/binary-float mixing, so sema
    // has already rejected any program that could reach that.
    if (l->getType() == decimalStructType()) {
        r = coerceNumericToType(r, decimalStructType(), rUns);
        if (r->getType() != decimalStructType()) return nullptr;
        return emitDecimalArith(op, l, r);
    }
    // §4.6 (#857): string compound concatenation when BOTH operands are string
    // pointers (e.g. an indexer/property `s[i] += t`). A string is a pointer, not
    // a number, so the integer checked-add intrinsic below emits malformed IR.
    // The main local/field `s += t` path intercepts before this with full sema
    // types (and the value-conversion / refcount discipline); this is the safety
    // net for the remaining call sites — value-correct concat. (Only fires for
    // `+`; other ops on a string pointer never typecheck.)
    if (op == "+" && l->getType()->isPointerTy() && r->getType()->isPointerTy())
        return emitStringConcat(l, r);
    if (fp) {
        if (op == "+") return builder_.CreateFAdd(l, r);
        if (op == "-") return builder_.CreateFSub(l, r);
        if (op == "*") return builder_.CreateFMul(l, r);
        if (op == "/") return builder_.CreateFDiv(l, r);
        return nullptr;
    }
    // §6.6/§6.2 (#1043, #2140): an ARITHMETIC compound assignment on an integer
    // LHS whose §6.2-promoted result width EXCEEDS the LHS width runs the op at
    // the promoted width and CHECKED-narrows back to the LHS type. Two triggers:
    // a SUB-i32 LHS (`byte`/`sbyte`/`short`/`ushort`) promotes to at least `int`
    // (#1043 — `byte b := 5; b += 300` must raise, not truncate 300→44); and ANY
    // LHS with a strictly WIDER RHS (`int += long`, `uint += ulong`,
    // `byte += long`) promotes to the RHS type (#2140). Below the promoted width
    // the #1042 tail's `CreateIntCast(r, l->getType())` silently TRUNCATED a wide
    // RHS — `i += 2^32L` dropped the high bits and stored 0 with no raise, and
    // `i += (2^32+65)L` stored a wrong in-range 65 — while the sub-i32 extend-to-
    // i32 emitted an INVALID narrowing ZExt/SExt on an i64 RHS (malformed IR /
    // assert crash). Same defect class #1996 fixed for `char` (0535081); this is
    // its integer-family sibling.
    //
    // Per §6.2 the arithmetic runs in the WIDER operand's type with THAT type's
    // signedness (`arithUns`): each operand is first extended to the promoted
    // width by ITS OWN signedness (LHS by `uns`, RHS by `rUns`), so a byte's
    // 0..255 value ZExts and a signed `long` RHS SExts. `byte(5) += -1L` then
    // computes signed `5 + (-1) = 4` (not a spurious unsigned overflow) and
    // narrows to `byte(4)`. The result is narrowed back to the LHS type by the
    // §5.2 CHECKED narrowing (`uns`): a trunc whose sign/zero re-extension differs
    // from the wide value is out of range → OverflowException (never a silent
    // truncation, §6.1). (`mod` has no compound form, §6.6; listed for parity.)
    //
    // §6.6/§6.5 (#2147): the non-shift BITWISE ops (`&`/`|`/`^`) take this same
    // branch when the RHS is strictly WIDER — they promote per §6.2 exactly like
    // arithmetic (§3.5: "operand-type combinations follow the numeric-promotion
    // rules in §6"), so `int & long : long` and the compound needs the §5.2
    // checked write-back: `int(-1) &= 2^32L` is SExt(-1) & 2^32 = 2^32, out of
    // int range → OverflowException. (The #1042 tail truncated the wide RHS
    // FIRST and stored 0 with no raise; `|`/`^` with set high bits, and `&`
    // against a sign-extended negative operand, likewise reached a wider-than-
    // LHS result silently truncated.) Unlike arithmetic, `&`/`|`/`^` cannot
    // overflow the PROMOTED width itself, so the recursed wide op is the plain
    // And/Or/Xor — only the final narrow back to T is checked. Two differences
    // from the arithmetic trigger: no i32 floor (a same-width pair keeps its
    // type — `byte ^ byte : byte`, §6.6's `checksum ^= b` cell — and the exact
    // LHS-width op needs no check), and SHIFTS stay on the LHS-width path
    // below: a shift's result already has the left operand's type (§6.5). The
    // count is NO LONGER masked — since #3045 (§6.5, v0.242) an out-of-domain
    // count is a fault, so a shift CAN raise OverflowException; the guard is
    // emitted by emitShiftCountInDomain above, ahead of the #1042 coercion.
    bool wideBitwise = (op == "&" || op == "|" || op == "^");
    if (l->getType()->isIntegerTy() && !l->getType()->isIntegerTy(1) &&
        r->getType()->isIntegerTy() && !r->getType()->isIntegerTy(1) &&
        (op == "+" || op == "-" || op == "*" || op == "/" || op == "mod" ||
         wideBitwise)) {
        llvm::Type* dstTy = l->getType();
        unsigned lBits = dstTy->getIntegerBitWidth();
        unsigned rBits = r->getType()->getIntegerBitWidth();
        unsigned promo = lBits;
        if (rBits > promo) promo = rBits;
        // §6.2/#1043: arithmetic on a sub-i32 LHS floors at i32 (`byte + int
        // literal` runs in int); bitwise does NOT — `byte & byte : byte` is
        // exact at the LHS width and §6.6's write-back never applies (#2147).
        if (promo < 32 && !wideBitwise) promo = 32;
        if (promo > lBits) {
            llvm::Type* wideTy = llvm::Type::getIntNTy(llvm_, promo);
            // §6.2: the op's type is the wider operand's — the RHS when it is
            // strictly wider, else the LHS (a sub-i32 LHS floored to i32, or a
            // same-width pair; §6.2 admits no mixed-signedness same-width pair, so
            // `uns` == `rUns` there).
            bool arithUns = (rBits > lBits) ? rUns : uns;
            auto widen = [&](llvm::Value* v, bool vUns) -> llvm::Value* {
                if (v->getType() == wideTy) return v;
                return vUns ? builder_.CreateZExt(v, wideTy)
                            : builder_.CreateSExt(v, wideTy);
            };
            llvm::Value* wide = emitCompoundOp(op, widen(l, uns), widen(r, rUns),
                                               /*fp=*/false, arithUns);
            if (!wide) return nullptr;
            // §5.2 checked narrowing: trunc, extend back with the LHS
            // signedness, compare to the wide value — a mismatch means out of
            // range.
            llvm::Value* trunc = builder_.CreateTrunc(wide, dstTy);
            llvm::Value* back = uns ? builder_.CreateZExt(trunc, wideTy)
                                    : builder_.CreateSExt(trunc, wideTy);
            emitGuardThrow(builder_.CreateICmpNE(back, wide),
                           "OverflowException");
            return trunc;
        }
    }
    // §6.5 / §6.6 (v0.242, #3045): `x <<= k` / `x >>= k` "inherit §6.5's
    // count-domain rule through the desugaring" (§6.6) — the count's domain is
    // `0 .. width−1` of the TARGET's type, and an out-of-domain runtime count
    // raises `OverflowException` (§31.4) instead of being reduced modulo the
    // width. This arm must run BEFORE the #1042 tail below, which coerces the
    // RHS to the LHS width: a `long` count of 2^40 truncates to 0 in i32 and
    // would then read as in-domain. (Before v0.242 the arm at the bottom of
    // this function masked the count `& (bits-1)`, the #1041 fix; the mask is
    // gone with the rule it implemented.)
    if ((op == "<<" || op == ">>") && l->getType()->isIntegerTy() &&
        r->getType()->isIntegerTy()) {
        llvm::Value* cnt = emitShiftCountInDomain(l, r, rUns);
        if (op == "<<") return builder_.CreateShl(l, cnt);
        return uns ? builder_.CreateLShr(l, cnt) : builder_.CreateAShr(l, cnt);
    }
    // #1042: the compound result has the LHS type, and the checked-arith
    // intrinsics / shifts below require both operands to share that integer
    // type. The RHS is lowered at its own natural width (an `int` literal/value
    // is i32), so a sub-i32 LHS (`byte`/`sbyte`/`short`/`ushort`) gave a width
    // mismatch — e.g. `byte b += 3` called `llvm.uadd.with.overflow.i8` with an
    // i32 arg → malformed IR. Coerce the RHS to the LHS integer type so the
    // operation runs at the LHS width with the correct (LHS-width) overflow
    // check — exactly how `b++`/`b--` build their `1` operand at the target
    // width (those already worked; the explicit `b += v` form crashed). A wider
    // LHS (`long x += short`) already arrives pre-widened (l == r), so this is a
    // no-op there.
    if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy() &&
        l->getType() != r->getType())
        r = builder_.CreateIntCast(r, l->getType(), /*isSigned=*/!uns);
    if (op == "+")
        return emitCheckedArith(uns ? llvm::Intrinsic::uadd_with_overflow
                                    : llvm::Intrinsic::sadd_with_overflow,
                                l, r);
    if (op == "-")
        return emitCheckedArith(uns ? llvm::Intrinsic::usub_with_overflow
                                    : llvm::Intrinsic::ssub_with_overflow,
                                l, r);
    if (op == "*")
        return emitCheckedArith(uns ? llvm::Intrinsic::umul_with_overflow
                                    : llvm::Intrinsic::smul_with_overflow,
                                l, r);
    if (op == "/" || op == "mod") {
        emitGuardThrow(builder_.CreateICmpEQ(r, zeroLike(r)),
                       "DivideByZeroException");
        if (uns)
            return op == "/" ? builder_.CreateUDiv(l, r)
                             : builder_.CreateURem(l, r);
        // Signed INT_MIN op -1 (§6.1): only division overflows; modulo is 0 and
        // must not raise. srem of INT_MIN,-1 is UB, so force divisor to 1 and
        // select 0 on that path. (The `mod` arm became reachable at v0.242
        // with the `mod=` compound form — §6.6/A.6, #3052; it was already
        // reachable through `x := x mod y` in the plain-assignment lowering.)
        llvm::Value* negOne = llvm::ConstantInt::getSigned(r->getType(), -1);
        llvm::Value* isMinNegOne =
            builder_.CreateAnd(builder_.CreateICmpEQ(l, minIntLike(r)),
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
    if (op == "&") return builder_.CreateAnd(l, r);
    if (op == "|") return builder_.CreateOr(l, r);
    if (op == "^") return builder_.CreateXor(l, r);
    // (§6.5's shift arm ran above, before the #1042 RHS coercion — see there.)
    return nullptr;
}


llvm::Type* Emitter::lowerType(const ast::Type* t) {
    if (!t) return llvm::Type::getVoidTy(llvm_);
    return lowerTypeRef(ctx_.resolvedType(*t));
}


bool Emitter::isNullablePrimTy(const sema::TypeRef* rt) {
    if (!rt || !rt->nullable ||
        rt->kind != sema::TypeRef::Kind::Primitive) return false;
    using P = sema::PrimitiveType;
    switch (rt->primitive) {
        case P::Bool:
        case P::Byte: case P::SByte:
        case P::Short: case P::UShort:
        case P::Int: case P::UInt:
        case P::Long: case P::ULong:
        case P::Float: case P::Double:
        // fix #849 / §7.1: `char` is a value type (lowered to i32), so `char?`
        // is a nullable VALUE type with the {i8 hasValue; i32} tagged-struct
        // representation, exactly like `int?`. It was previously excluded
        // (lumped with the reference primitives String/Object) → `char?` was
        // classified as NEITHER a nullable value NOR a nullable reference, so
        // its representation was inconsistent across code paths: a null `char?`
        // interpolated as a NUL byte instead of "null", and `c ?? x` crashed
        // with a PHI type mismatch (the struct vs the bare i32).
        case P::Char:
        // per §4.4 / fix #663: decimal is a 16-byte struct value type
        // (§4.4, #521) — not a scalar, but still a nullable value type that
        // needs the {i8 hasValue; %bz.decimal value} tagged-struct representation
        // (exactly as int? / double? use {i8, i32} / {i8, double}).
        // Before this fix `decimal?` fell through to the default → isNullableValueTy
        // returned false → `return null` emitted `ret ptr null` against the struct
        // return type (IR-verify abort, r04e), and the null tag was never set
        // (null-comparison always failed, r04b/r04g).
        case P::Decimal:
            return true;
        default: return false;  // String, Char, Object, None
    }
}


// per §10 / issue #364: a NULLABLE ENUM (a nullable Named TypeRef whose
// declaration is an enum) is a value type that lacked a null representation —
// a non-nullable enum lowers to its underlying integer (i32), so `enum?` stayed
// i32 and `null` collided with the value-0 member. It now lowers, like the
// value primitives, to the tagged {i8 hasValue; i32 value} struct, so null is
// representable and distinct from every enum value.
bool Emitter::isNullableEnumTy(const sema::TypeRef* rt) {
    return rt && rt->nullable &&
           rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Enum;
}


// per §7.2 / §8.6 / issue #416: a NULLABLE RECORD (a nullable Named TypeRef
// whose declaration is a record) is a value type that lacked a null
// representation — a non-nullable record lowers to a bare by-value struct
// (§9.4), so `record?` stayed the bare struct and `null` collided with a real
// value. It now lowers, like the value primitives and enums, to the tagged
// {i8 hasValue; <record payload>} struct, so null is representable and distinct
// from every record value (lowerTypeRef / nullableValueStructTy below).
bool Emitter::isNullableRecordTy(const sema::TypeRef* rt) {
    return rt && rt->nullable &&
           rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Record;
}


// fix #2447 / §35.9 / §7.2: a NULLABLE CHOICE (a nullable Named TypeRef
// whose declaration is a choice) is a value type that lacked a null
// representation — see the declaration comment in LlvmBackend_impl.hpp for
// the full rationale. The choice analogue of isNullableRecordTy just above.
bool Emitter::isNullableChoiceTy(const sema::TypeRef* rt) {
    return rt && rt->nullable &&
           rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Choice;
}


// per §2.5–§2.6 / issue #1954: is rt a NULLABLE VALUE-BUILTIN (DateTime/TimeSpan/
// DateOnly/TimeOnly/Guid)? See the declaration comment in LlvmBackend_impl.hpp for
// why these need the tagged-struct nullable lowering (esp. the multi-field Guid
// {i64,i64}, whose bare lowering has no null representation → the malformed
// nullable-wrapper GEP crash). valueBuiltinName resolves the name across all three
// TypeRef shapes ignoring nullability, so gate on rt->nullable here.
bool Emitter::isNullableValueBuiltinTy(const sema::TypeRef* rt) {
    return rt && rt->nullable && valueBuiltinName(rt) != nullptr;
}


// fix #2763 / §7.2 / §2.5–§2.6 / §4.14–§4.15 / §2.15 / §9.8 / §31.4: is rt a
// NULLABLE VALUE-STRUCT BUILTIN? Spans (§4.14/§4.15/§3.3) — Span/ReadOnlySpan/
// ArraySpan/ReadOnlyArraySpan/ReadOnlyStringSpan — and JsonOptions (§2.15),
// ExceptionInfo (§31.4), and Range (§9.8) all lower to bare STRUCT value types
// where no field records "this is null"; a `Range<int>? := 1..5;` would
// overwrite the 24-byte Range payload into a placeholder slot and the
// `= null` check would read a wrong element. The same family as the five
// value-builtins (isNullableValueBuiltinTy) but with their OWN set — adding
// them to valueBuiltinBoxNameStr (the BOX-key set) would route their Object
// boxing to a missing primBoxDesc_ entry and silently break boxing. Resolves
// the name across the three TypeRef shapes (declaration, astNode head,
// importedName) the same way isRangeType / isSpanLikeTypeName do, and gates on
// rt->nullable so the non-nullable case keeps its bare-struct lowering.
bool Emitter::isNullableValueStructBuiltinTy(const sema::TypeRef* rt) {
    if (!rt || !rt->nullable || rt->kind != sema::TypeRef::Kind::Named)
        return false;
    std::string_view n;
    if (rt->declaration) {
        n = rt->declaration->name;
    } else if (rt->astNode && rt->astNode->name &&
               !rt->astNode->name->parts.empty()) {
        n = rt->astNode->name->parts[0];
    } else if (!rt->importedName.empty()) {
        n = rt->importedName;
    } else {
        return false;
    }
    // §4.14 / §4.15 / §3.3 — the array-span value-struct family (owning +
    // borrowing + the string-backed read-only twin); all share the
    // arraySpanStructType {ptr,i32,i32} shape.
    if (n == "Span" || n == "ReadOnlySpan" ||
        n == "ArraySpan" || n == "ReadOnlyArraySpan" ||
        n == "ReadOnlyStringSpan") return true;
    // §9.8 — Range<T> with its {i64,i64,i64,i8} struct shape.
    if (n == "Range") return true;
    // §2.15 — JsonOptions, the 5-field compiler-known value record.
    if (n == "JsonOptions") return true;
    // §31.4 — ExceptionInfo, the {ptr,ptr,ptr} causechain element.
    if (n == "ExceptionInfo") return true;
    return false;
}


// per §8.3/§9.1: a nullable reference type lowers to a plain `ptr` whose null
// is a real null pointer (no hasValue tag). The synthesized value-semantics
// members must null-guard a field of such a type before dereferencing it.
bool Emitter::isNullableRefTy(const sema::TypeRef* rt) {
    if (!rt || !rt->nullable) return false;
    using K = sema::TypeRef::Kind;
    using P = sema::PrimitiveType;
    // string? and Object? are reference primitives that lower to ptr.
    if (rt->kind == K::Primitive)
        return rt->primitive == P::String || rt->primitive == P::Object;
    // Nullable class/interface field (a named class or interface type).
    if (rt->kind == K::Named && rt->declaration)
        return rt->declaration->kind == ast::TypeDeclKind::Class ||
               rt->declaration->kind == ast::TypeDeclKind::Interface;
    return false;
}


llvm::StructType* Emitter::nullablePrimStructTy(sema::PrimitiveType pt) {
    return llvm::StructType::get(llvm_, {nullablePrimHasValTy(), lowerPrimitive(pt)});
}


llvm::Value* Emitter::wrapNullablePrim(sema::PrimitiveType pt, llvm::Value* val) {
    auto* st = nullablePrimStructTy(pt);
    auto* hasValTy = nullablePrimHasValTy();
    llvm::Value* result =
        builder_.CreateInsertValue(llvm::UndefValue::get(st),
            llvm::ConstantInt::get(hasValTy, 1), {0u});
    return builder_.CreateInsertValue(result, val, {1u});
}


llvm::Type* Emitter::lowerTypeRef(const sema::TypeRef* rt) {
    if (!rt) return i32();
    // CG-nullable: nullable value type (prim or enum) → {i8 hasValue; T value}.
    // For an enum the value slot is its underlying integer (i32 — CG44); per
    // issue #364 this gives `enum?` a null distinct from the value-0 member.
    if (isNullableValueTy(rt)) return nullableValueStructTy(rt);
    // fix #568 / §14.1 / §7.1: a nullable BARE TYPE-PARAMETER (`T?` where `T` is
    // a generic param with no declaration) inside a generic method body.
    // isNullableValueTy is false for it (`T` resolves to no known value type), so
    // without this it would fall through to the generic-subst branch below, which
    // resolves `T` → its concrete LLVM type and DROPS the nullability — typing a
    // `T?` (T=int) return as i32. The body's `return null` then emits
    // `ret ptr null` against an i32 return type (IR-verify failure). Two shapes:
    //   * MONOMORPHIZED body (genericSubstSemaTy_ active): when the concrete
    //     substitution is a VALUE type, lower to the SAME nullable struct the
    //     value-type `T?` would produce — resolve the parameter to its concrete
    //     sema type (the #567 mechanism) and lower its nullable variant. A param
    //     bound to a REFERENCE type (string/class) keeps the reference `?`
    //     lowering (a plain ptr) — handled by the fall-through.
    //   * UNSPECIALIZED TEMPLATE body (no active subst, emitFunctionBody — the
    //     "dead" template #567 also has to keep IR-valid): the concrete T is
    //     unknown, so lower `T?` to the placeholder nullable value struct
    //     {i8, i32}. `return null` then emits a valid zeroinitializer struct and
    //     `return v` (v:T lowered to the i32 placeholder) wraps as tag=1. The
    //     template is never called; only the per-instantiation specialization is.
    if (rt->nullable && rt->kind == sema::TypeRef::Kind::Named &&
        !rt->declaration && rt->astNode && rt->astNode->name &&
        rt->astNode->name->parts.size() == 1 &&
        !isBuiltinTypeRefName(rt->headName())) {
        if (!genericSubstSemaTy_.empty()) {
            sema::TypeRef nv;
            if (nullableTypeParamConcrete(rt, nv)) {
                if (isNullableValueTy(&nv)) return nullableValueStructTy(&nv);
                // fix #2743 / §14.1: the substitution may map this parameter onto
                // ANOTHER, still-unbound type parameter (`T → U`). That happens
                // when a specialization of an inner generic is built lazily from
                // inside an OUTER generic's unspecialized TEMPLATE body — e.g.
                // `Outer<U>.Head(): U?` forwarding `return g.Head();` to
                // `Inner<T>.Head(): T?` builds `Inner$Head$spec` under {T → U}.
                // `nv` is then not a value type, but it is NOT a reference type
                // either: T's concrete form is still unknown, so the TEMPLATE rule
                // applies exactly as in the no-binding branch below. Falling
                // through instead resolved `T` to U's PLACEHOLDER LLVM type (i32)
                // and DROPPED the nullability — the pseudo-specialization was
                // typed `i32` while its body's `return null` emitted `ret ptr null`
                // (IR-verify failure, `bezelc -c` exit 1). The same placeholder
                // struct the template `bz$M.Inner$Head` already uses keeps the
                // signature and every return in that body consistent.
                if (isBareTypeParamRef(&nv))
                    return nullablePrimStructTy(sema::PrimitiveType::Int);
                // resolved to a reference type → plain ptr `?` lowering.
            } else {
                // a type param with no active binding — fall to template rule.
                return nullablePrimStructTy(sema::PrimitiveType::Int);
            }
        } else {
            // unspecialized template body: placeholder nullable value struct.
            return nullablePrimStructTy(sema::PrimitiveType::Int);
        }
    }
    switch (rt->kind) {
        case sema::TypeRef::Kind::Unit:
            // §4.7: the NON-nullable unit type. `void` is right here and only
            // here — it is what a unit-returning function returns. It is NOT a
            // valid `alloca`/parameter/struct-member type, so a storage position
            // must substitute the one-byte placeholder (`unitPlaceholderTy`).
            //
            // ⚠ WHICH positions do, stated as a LIST rather than as "every one",
            // because the list is not complete and a reader must not take the
            // stronger claim on trust (#3098 follow-up review): `lowerLocalDecl`
            // (the #1619 guard), the method/constructor signature builders and
            // their imported faces (`lowerParamType` / `lowerParamTypeRef`), the
            // lambda parameter list, and — structurally, so a missed site is a
            // placeholder rather than a hardware trap — `entryAlloca` itself.
            // Five signature-building sites still call the RAW lowerings and
            // would type a `()` slot `void`: the extern/FFI parameter list and
            // the static-auto-property global (this file's `declareFunctions` /
            // static-global loop), the imported indexer's parameter and `value`
            // lists, and the function-VALUE call's own parameter list
            // (LlvmBackend_Calls2.cpp — that one is #3137). All are pre-existing
            // and none is reachable by a program measured so far; `entryAlloca`
            // keeps each of them out of the SIGILL class regardless.
            //
            // fix #3095 / §4.7: `()?` never reaches here — `isNullableValueTy`
            // at the top of this function claims it first and returns the
            // tagged `{i8 hasValue, i8}` optional (nullableValueStructTy).
            // Before that, this arm answered `void` for `()?` as well, which
            // SIGILL'd `bezelc` at a `()?` parameter (CreateAlloca(void)).
            return llvm::Type::getVoidTy(llvm_);
        case sema::TypeRef::Kind::Primitive:
            return lowerPrimitive(rt->primitive);
        case sema::TypeRef::Kind::Named:
            // CG-generic: a Named type with null declaration may be a generic
            // type parameter (e.g. T in Box<T>). If there's an active
            // substitution and the single-part name is a bound parameter,
            // resolve to its concrete type.
            //
            // issue #420: only RETURN here when the name actually resolves to a
            // bound parameter — fall through otherwise so the builtin / FFI /
            // declaration checks below run. Previously this returned i32() for
            // ANY no-declaration Named type while a substitution was active,
            // which clobbered the heap-collection (Array<T>/OwningArray<T>/…),
            // dictionary, and FFI (IntPtr/CString/…) lowerings to i32 inside a
            // monomorphized generic body. For a generic method returning
            // `owned Array<T>` (§14.1, lowered by emitGenericSpecialization),
            // the specialized function was then typed to return i32; the bare
            // bz_array_t* pointer was truncated to 32 bits on the return and
            // re-extended on the caller-side store, so the §28.3 using-var
            // scope-end disposal loaded a corrupt pointer and crashed in
            // bz_array_free. A bare unresolved type parameter (`T` with no
            // substitution) still reaches the value-kind i32 fallback at the
            // end of this switch, so that behavior is unchanged.
            if (!rt->declaration && !genericSubst_.empty() &&
                rt->astNode && rt->astNode->name &&
                rt->astNode->name->parts.size() == 1) {
                auto sit = genericSubst_.find(rt->headName());
                if (sit != genericSubst_.end())
                    return sit->second;
            }
            // #428 / §14.1 / §2.5: an imported generic-record field whose
            // type is a type parameter carries NO astNode (there is no AST
            // in the importer), but the parameter name is in importedName
            // (set by ModuleLoader.cpp Loader::convert for the Named/no-decl
            // case). A single-part name in importedName with no declaration
            // and no astNode is a type-parameter reference — look it up in the
            // active genericSubst_ exactly as the astNode path above does.
            if (!rt->declaration && !genericSubst_.empty() &&
                rt->astNode == nullptr && !rt->importedName.empty()) {
                auto sit = genericSubst_.find(rt->importedName);
                if (sit != genericSubst_.end())
                    return sit->second;
            }
            // §32.5 / §32.7 / stdlib §10: FFI-boundary types. IntPtr and
            // UIntPtr are pointer-sized integers — on the host target that is
            // 64-bit, so they lower to i64. CString/WString/RawBytes are
            // opaque pointers at the C boundary (char*, wchar*, void*).
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty()) {
                auto fn = rt->headName();
                if (fn == "IntPtr" || fn == "UIntPtr")
                    return i64();  // per §32.7: pointer-sized integer (i64 host)
                if (fn == "CString" || fn == "WString" || fn == "RawBytes")
                    return ptrTy();  // per §32.5: opaque C-boundary pointer
            }
            // §2.5: DateTime and TimeSpan are value types stored as a scalar
            // int64 tick count — NOT heap-allocated pointers. Lower to i64 so
            // they pass through function arguments and return values correctly.
            // This applies to both the "no-declaration" (parsed annotation)
            // and the "builtinClassSym" (returned by Now/UtcNow/From*) shapes.
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty()) {
                auto n0 = rt->headName();
                if (n0 == "DateTime" || n0 == "TimeSpan") return i64();
                // per §2.5 / #1357: DateOnly and TimeOnly are i32 value types.
                // DateOnly: day number since 0001-01-01; TimeOnly: ms since midnight.
                if (n0 == "DateOnly" || n0 == "TimeOnly") return i32();
            }
            if (rt->declaration && !rt->declaration->decl &&
                (rt->declaration->name == "DateTime" ||
                 rt->declaration->name == "TimeSpan")) {
                return i64();
            }
            // per §2.5 / #1357: DateOnly / TimeOnly — builtinClassSym shape
            // (TypeRef produced by DateOnly.Parse / TimeOnly.Parse / ToDateTime
            // return types, and DateTime.Date / DateTime.TimeOfDay properties).
            // Must be i32, not ptrTy() — they are scalar value types, not heap ptrs.
            if (rt->declaration && !rt->declaration->decl &&
                (rt->declaration->name == "DateOnly" ||
                 rt->declaration->name == "TimeOnly")) {
                return i32();
            }
            // fix #499 / §9.8: Range<T> is a compiler-intrinsic value-type
            // record. Lower it to { i64 start, i64 end, i64 step, i8 flags }
            // where flags bit 0 = IsEndInclusive (1 = `..`, 0 = `..<`).
            // Using i64 for start/end (independent of T's width) keeps the
            // struct layout uniform across Range<int>/Range<long>/etc.
            if (rt->declaration && !rt->declaration->decl &&
                rt->declaration->name == "Range") {
                return rangeStructType();
            }
            // §3 stdlib: collection and IO built-in classes (no user TypeDecl).
            // All are heap-allocated opaque pointers to runtime structs.
            // per stdlib §2.6: Guid is a 128-bit value type — { i64 lo, i64 hi }.
            // Unlike collections (heap pointers), a Guid lowers to the inline struct.
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty() &&
                rt->headName() == "Guid") {
                return guidStructType();
            }
            // §4.15 (#549) / §4.14 (#779): ArraySpan/ReadOnlyArraySpan (borrowing)
            // and Span/ReadOnlySpan (owning) are value structs
            // { ptr source, i32 start, i32 length } — no heap pointer.
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty() &&
                (rt->headName() == "ArraySpan" ||
                 rt->headName() == "ReadOnlyArraySpan" ||
                 rt->headName() == "Span" ||
                 rt->headName() == "ReadOnlySpan" ||
                 // §3.3 (#883): ReadOnlyStringSpan shares the {ptr,i32,i32} shape
                 // (field 0 is a string pointer, not an array pointer).
                 rt->headName() == "ReadOnlyStringSpan")) {
                return arraySpanStructType();
            }
            // §2.15 / issue #687: JsonOptions is a compiler-known value record.
            // { i1 Indented, i32 IndentWidth, i1 AllowTrailingCommas,
            //   i1 IgnoreUnknownFields, i32 MaxDepth }
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty() &&
                rt->headName() == "JsonOptions") {
                return jsonOptionsStructType();
            }
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty()) {
                auto n = rt->headName();
                // #1414 stage 3: the shared heap-pointer reference-builtin name
                // set now lives in the registry (one home, snapshot-pinned).
                // This is the miss-a-list class behind #1390 (serializer pointer
                // truncation) and #1406 (SortedDictionary i32). §4.14
                // OwningBuffer<T> (bz_array_t*) is an astNode-path-only addition
                // — the FFI class names on the imported path have no local
                // astNode counterpart (they are handled by the i64/ptr branch
                // above) — so it stays an explicit delta here.
                if (common::isHeapPointerReferenceBuiltin(n) ||
                    n == "OwningBuffer") {
                    return ptrTy();
                }
            }
            // §6.1 / §30.8: a CancellationToken-typed variable produced by
            // cts.Token has a non-null builtinClassSym declaration (used to
            // carry the type name through sema); recognise it by declaration name
            // so lowerTypeRef returns ptrTy() for it.
            if (rt->declaration && !rt->declaration->decl &&
                (rt->declaration->name == "CancellationToken" ||
                 rt->declaration->name == "CancellationTokenSource" ||
                 // stdlib §6.10 (v0.175, #808): TaskScope via builtinClassSym.
                 rt->declaration->name == "TaskScope")) {
                    return ptrTy();
            }
            // per stdlib §2.6: Guid typed via builtinClassSym (declaration->name ==
            // "Guid", null decl) → value struct { i64, i64 }.
            if (rt->declaration && !rt->declaration->decl &&
                rt->declaration->name == "Guid") {
                return guidStructType();
            }
            // §2.5 / §29.3 (issue #495): imported TypeRefs for builtin stdlib
            // types (IEnumerable<T>, Array<T>, Task, etc.) carry the type name
            // in importedName (set by ModuleLoader — no astNode exists for
            // imported types). Recognise all the heap-allocated builtin names here
            // so they lower to ptrTy() rather than the i32 default.  The same
            // name set as the !rt->declaration && rt->astNode path above — both
            // are pointer-sized heap objects.  Value-type builtins (DateTime,
            // TimeSpan, Guid, Range) must be listed first (they fall through here
            // only if importedName is set AND the guards above did not match).
            if (!rt->declaration && !rt->astNode && !rt->importedName.empty()) {
                auto n = rt->importedName;
                // Value types — mirror the astNode guards above.
                if (n == "DateTime" || n == "TimeSpan") return i64();
                // fix #2829 / §32.7 / stdlib §10.0: IntPtr/UIntPtr are
                // pointer-sized INTEGER value types (i64 on the host), not
                // C-boundary reference types — mirror the astNode arm at
                // line ~944-945 (IntPtr/UIntPtr → i64()).
                if (n == "IntPtr" || n == "UIntPtr") return i64();
                if (n == "Guid")  return guidStructType();
                if (n == "Range") return rangeStructType();
                // §4.15 (#549) / §4.14 (#779) / §3.3 (#883): span value structs.
                if (n == "ArraySpan" || n == "ReadOnlyArraySpan" ||
                    n == "Span" || n == "ReadOnlySpan" ||
                    n == "ReadOnlyStringSpan")
                    return arraySpanStructType();
                // Heap-pointer types — all stdlib collection/interface/task names.
                // #1414 stage 3: the shared set lives in the registry (one home,
                // snapshot-pinned). The FFI class names are an imported-path-only
                // delta (their astNode counterparts lower to i64/ptr in the
                // FFI branch above, which imported types never reach), so they
                // stay explicit here. (This path has no OwningBuffer entry —
                // preserved: it was never in the imported list.)
                if (common::isHeapPointerReferenceBuiltin(n) ||
                    // §32.5 / stdlib §10: imported FFI-boundary REFERENCE types
                    // — opaque pointers at the C boundary (char*, wchar*, void*).
                    // IntPtr/UIntPtr are deliberately absent: per §32.7 and stdlib
                    // §10.0 they are pointer-sized INTEGER value types (i64 on the
                    // host), lowered above by the value-types branch.
                    // fix #2829 / §32.7 / stdlib §10.0: lumping IntPtr/UIntPtr in
                    // here returned `ptr` on the imported-name path, mismatching
                    // the astNode-path `i64` and tripping ICmpInst::AssertOK /
                    // cast<IntegerType> at the caller.
                    n == "CString" || n == "WString" || n == "RawBytes") {
                    return ptrTy();
                }
            }
            // #405 / §31.1 / §31.4: a built-in exception type (Exception or a
            // §31.4 subtype) has no user TypeDecl but IS a reference type — a
            // heap-allocated object (vtable + message). It must lower to a
            // pointer; without this it fell through to the i32 value default,
            // truncating the exception pointer wherever a built-in exception was
            // a field / local / parameter / return (so an owned field/local of
            // built-in-exception type stored a clobbered half-pointer that the
            // §28.4 / §28.10 auto-dispose then freed at the wrong address).
            //
            // #1621: the name comes from `headName()` (astNode's syntactic name
            // when the type was written directly in source, falling back to
            // `importedName`) rather than requiring `rt->astNode` — a multi-type
            // catch-arm binding's nearest-common-base can be a SYNTHETIC
            // built-in-ancestor TypeRef (`ExprTC::builtinAncestorTypeRef`, minted
            // when the NCB is reached only via the built-in exception hierarchy
            // table, e.g. the `Exception` common to sibling `OverflowException`
            // and `ArgumentException`) which carries no astNode at all; without
            // this fallback it fell through to the i32 default below — the exact
            // pointer-truncation bug this check exists to prevent.
            if (!rt->declaration) {
                auto exName = rt->headName();
                if (!exName.empty() &&
                    builtinVtable_.find(std::string(exName)) !=
                        builtinVtable_.end())
                    return ptrTy();
            }
            if (rt->declaration &&
                (rt->declaration->kind == ast::TypeDeclKind::Class ||
                 rt->declaration->kind == ast::TypeDeclKind::Interface)) {
                return ptrTy();  // reference types are heap pointers
            }
            // §9.4: a record is a value type, lowered to a first-class
            // LLVM struct value.
            if (rt->declaration &&
                rt->declaration->kind == ast::TypeDeclKind::Record) {
                // issue #460: a CONCRETE generic-record instantiation appearing
                // as a type annotation (a method return / parameter / field of
                // `Box<string>`) must lower to the SAME specialized struct the
                // construction + field-access sides produce, or the function
                // signature ({i32} placeholder) disagrees with the body
                // (specialized) and the return/store IR is malformed. Set up a
                // temporary substitution from the record's concrete type args
                // (resolved under any OUTER subst, so `Box<T>` inside a
                // `Pair<string>` body maps T → its concrete type), and lower
                // the record struct under it. A non-generic record (no
                // genericArgs) keeps the unspecialized struct.
                // fix #3143 (§14.1 / §2.5, v0.254): parameter names through the
                // one both-sources walk. Read off the AST decl alone, an
                // IMPORTED generic record named as a method PARAMETER or RETURN
                // type fell to `recordStruct(...)` below — the erased
                // `%record.R = { i32, i32 }` placeholder — while every other
                // spelling of the same value in the same program produced
                // `%record.R$spec0`: measured on a real two-module build as
                // "Function return type does not match operand type of return
                // inst!" and "Call parameter type does not match function
                // signature!".
                const std::vector<std::string_view> rGpNames =
                    rt->genericArgs.empty()
                        ? std::vector<std::string_view>{}
                        : sema::genericParamNames(rt->declaration);
                if (!rGpNames.empty() &&
                    rGpNames.size() == rt->genericArgs.size()) {
                    // fix #428 phase a / §14.2 / §9.4: a GENERIC RECORD's type
                    // annotation (`Box<T>` as a method return or param type) should
                    // lower to the SAME struct the body will use.  When all type
                    // args are UNRESOLVED type-parameters (no declaration, single-
                    // part name, not bound in the outer genericSubst_), lowering
                    // each arg with the current (possibly empty) substitution
                    // produces the i32 placeholder — identical in value to the
                    // concrete `int` arg but a different LLVM named struct type
                    // (e.g. %record.Box$spec0 vs %record.Box).  The template body
                    // uses the UNSPECIALIZED %record.Box (classStruct with no
                    // active subst), so the two differ and LLVM rejects the `ret`
                    // instruction.  Skip specialization when every arg is still an
                    // unbound placeholder; the caller (declareFunctions, template
                    // body) is building the unspecialized template and must use the
                    // same base struct as the template body.
                    bool allUnbound = true;
                    for (const auto& arg : rt->genericArgs) {
                        if (!arg) { allUnbound = false; break; }
                        // An unbound type param: Named kind, no declaration,
                        // single-part name, NOT bound in the current outer subst.
                        // issue #787: a builtin collection (`Array<int>`) is also
                        // Named/declaration-less/single-part ("Array"); it carries
                        // its own generic args, so it is a concrete type, NOT an
                        // unbound parameter (a bare `T` has no generic args). Kept
                        // in sync with the mirror check in LlvmBackend_Exprs.cpp
                        // (lowerNew record construction).
                        if (arg->kind != sema::TypeRef::Kind::Named ||
                            arg->declaration ||
                            !arg->astNode || !arg->astNode->name ||
                            arg->astNode->name->parts.size() != 1 ||
                            !arg->genericArgs.empty() ||
                            genericSubst_.count(arg->headName())) {
                            allUnbound = false;
                            break;
                        }
                    }
                    if (allUnbound)
                        return recordStruct(rt->declaration);  // unspecialized

                    auto savedSubst = genericSubst_;
                    auto savedSemaTy = genericSubstSemaTy_;
                    std::unordered_map<std::string_view, llvm::Type*> newSubst;
                    std::unordered_map<std::string_view, const sema::TypeRef*>
                        newSema;
                    for (std::size_t gi = 0; gi < rGpNames.size(); ++gi) {
                        newSubst[rGpNames[gi]] =
                            lowerTypeRef(rt->genericArgs[gi]);
                        // §14.1 (#766): resolve a generic ARGUMENT that is itself
                        // a bare type parameter of an enclosing scope (the `T` in
                        // `new R<T>()` inside `Make<T>()`) through the active
                        // outer substitution, so the inner record's sema-type map
                        // holds the CONCRETE type (`int`), not the unresolved
                        // `T`. Otherwise the field layout uses the i32 placeholder
                        // while the value is the 8-byte nullable struct → an
                        // 8-into-4 stack write (SIGSEGV). A concrete arg is
                        // returned unchanged.
                        newSema[rGpNames[gi]] =
                            resolveTypeParamRef(rt->genericArgs[gi]);
                    }
                    genericSubst_ = std::move(newSubst);
                    genericSubstSemaTy_ = std::move(newSema);
                    llvm::StructType* spec = recordStruct(rt->declaration);
                    genericSubst_ = std::move(savedSubst);
                    genericSubstSemaTy_ = std::move(savedSemaTy);
                    return spec;
                }
                return recordStruct(rt->declaration);
            }
            // §10.3 (#740): an enum lowers to its underlying integer type
            // (default i32; `: byte|short|long|...` widen/narrow accordingly).
            if (rt->declaration &&
                rt->declaration->kind == ast::TypeDeclKind::Enum)
                return enumLlvmType(rt->declaration);
            // §35 (v0.200, #2045): a choice lowers to a value-type struct
            // `{ i32 tag, case0_payload, case1_payload, ... }` — see
            // choiceStruct for the layout rationale. This is the
            // non-nullable form; the nullable `C?` wraps this struct in
            // nullableValueStructTy's `{ i8 hasValue, <choice struct> }` —
            // fix #2447 / §35.9: this branch is reached ONLY for a
            // non-nullable choice, because isNullableValueTy (consulted at
            // the top of this function) now recognizes a nullable choice
            // via isNullableChoiceTy and routes it through
            // nullableValueStructTy before control ever reaches here. Before
            // #2447, isNullableValueTy did NOT recognize choices, so a
            // nullable choice fell through to THIS branch regardless of
            // `rt->nullable` — returning the bare struct with no null
            // representation, contradicting this very comment (a
            // long-standing aspirational note that was never actually
            // wired up) and crashing the first time downstream code tested
            // a (non-existent) hasValue tag.
            if (rt->declaration &&
                rt->declaration->kind == ast::TypeDeclKind::Choice) {
                // #2047: a CONCRETE generic-choice annotation (`Result<C,
                // string>` as a local/param/field type) must lower to the SAME
                // specialized struct construction/match/disposal produce — mirror
                // the record branch above: install a temporary substitution from
                // the choice's concrete type args (resolved under any outer
                // subst) and lower under it, unless every arg is still an unbound
                // template parameter (then the caller is the unspecialized
                // template and must share its base struct).
                // fix #3143 (§14.1 / §2.5, v0.254): parameter names through the
                // one both-sources walk — the choice twin of the record branch
                // above, so an imported generic choice annotation lowers to the
                // same specialized struct its construction/match sides produce.
                const std::vector<std::string_view> cGpNames =
                    rt->genericArgs.empty()
                        ? std::vector<std::string_view>{}
                        : sema::genericParamNames(rt->declaration);
                if (!cGpNames.empty() &&
                    cGpNames.size() == rt->genericArgs.size()) {
                    bool allUnbound = true;
                    for (const auto& arg : rt->genericArgs) {
                        if (!arg) { allUnbound = false; break; }
                        if (arg->kind != sema::TypeRef::Kind::Named ||
                            arg->declaration ||
                            !arg->astNode || !arg->astNode->name ||
                            arg->astNode->name->parts.size() != 1 ||
                            !arg->genericArgs.empty() ||
                            genericSubst_.count(arg->headName())) {
                            allUnbound = false;
                            break;
                        }
                    }
                    if (allUnbound)
                        return choiceStruct(rt->declaration);  // unspecialized
                    auto savedSubst = genericSubst_;
                    auto savedSemaTy = genericSubstSemaTy_;
                    std::unordered_map<std::string_view, llvm::Type*> newSubst;
                    std::unordered_map<std::string_view, const sema::TypeRef*>
                        newSema;
                    for (std::size_t gi = 0; gi < cGpNames.size(); ++gi) {
                        newSubst[cGpNames[gi]] =
                            lowerTypeRef(rt->genericArgs[gi]);
                        newSema[cGpNames[gi]] =
                            resolveTypeParamRef(rt->genericArgs[gi]);
                    }
                    genericSubst_ = std::move(newSubst);
                    genericSubstSemaTy_ = std::move(newSema);
                    llvm::StructType* spec = choiceStruct(rt->declaration);
                    genericSubst_ = std::move(savedSubst);
                    genericSubstSemaTy_ = std::move(savedSemaTy);
                    return spec;
                }
                return choiceStruct(rt->declaration);
            }
            // fix #1220 (residual) / stdlib §4.8 / §5 / issue #2794:
            // DictionaryEntry<K,V> is the element type for a Dictionary LINQ
            // pipeline. Pre-#2794 it was a synthetic Named TypeRef
            // (importedName="DictionaryEntry", no declaration/astNode) with
            // genericArgs=[K,V]; #2794 added it to `builtinTypeArity` so it
            // resolves as a WRITTEN type annotation too — that written form
            // carries an astNode, so the guard had to be relaxed to match
            // either path via headName() (which reads astNode->name->parts[0]
            // for written refs and falls back to importedName for synthetic).
            // DrainedEntry<K,V> is the OwningDictionary.Drain() iterator's
            // element type — same shape, same lowering. Both lower as a
            // 2-field struct {K, V} — the layout lowerDictEntryFor,
            // lowerDictDrainFor, and lowerExpr's DictionaryEntry GEP path
            // expect. Without this it fell to `i32`, so lowerLazyLinqFor
            // allocated a 4-byte slot and the loop-body GEP on the
            // DictionaryEntry fields crashed or loaded garbage.
            if (!rt->declaration &&
                (rt->headName() == "DictionaryEntry" ||
                 rt->headName() == "DrainedEntry") &&
                rt->genericArgs.size() >= 2 &&
                rt->genericArgs[0] && rt->genericArgs[1]) {
                llvm::Type* kTy = lowerTypeRef(rt->genericArgs[0]);
                llvm::Type* vTy = lowerTypeRef(rt->genericArgs[1]);
                if (kTy && vTy)
                    return llvm::StructType::get(llvm_, {kTy, vTy});
            }
            // §31.4: ExceptionInfo(TypeName, Message, StackTrace) — the built-in
            // record element of Exception.CauseChain. No user TypeSymbol
            // (declaration == null); recognise it by name (astNode or
            // importedName == "ExceptionInfo") and lower it to three consecutive
            // bz_string* — a 24-byte {ptr,ptr,ptr} value, the layout the runtime
            // snapshot (bz_exc_causechain_link) writes and the field-access
            // codegen reads. Gives Array<ExceptionInfo> an element size of 24.
            if (!rt->declaration &&
                ((rt->astNode && rt->astNode->name &&
                  !rt->astNode->name->parts.empty() &&
                  rt->astNode->name->parts.back() == "ExceptionInfo") ||
                 rt->importedName == "ExceptionInfo")) {
                return llvm::StructType::get(llvm_, {ptrTy(), ptrTy(), ptrTy()});
            }
            // stdlib §5 (#758): a lazy-LINQ enumerable sentinel TypeRef (Named
            // kind, null declaration/astNode, empty importedName, non-empty
            // genericArgs) is a heap `bz_lazy_t*`, not a value type — it falls
            // through every guard above to this value-kind fallback and would
            // wrongly lower to i32, declaring a SelectMany-style selector lambda
            // `(n) => arr.Where(...)` with an i32 return while its body returns
            // a `ptr` (the IR_FAIL "return type does not match operand type").
            // Lower it to a pointer. // per stdlib §5
            if (isLazyEnumerableType(rt)) return ptrTy();
            // fix #3577 (§2.8.3 rule 3, v0.286): the RESIDUE of the path
            // #3540 closed for the §11.6 case. A `Kind::Named` TypeRef that
            // survives every prior arm — no Class/Interface/Record
            // declaration, no builtin name match (DateTime/TimeSpan/Guid/
            // Range/Span/.../isHeapPointerReferenceBuiltin/builtinVtable_/
            // lazy-enumerable), no generic-subst binding — and carries an
            // IMPORTED QUALIFIED PATH NAME (importedName non-empty AND
            // contains a `.`) is a type from another module that the
            // loader could not resolve. Silently lowering it to i32 is the
            // bug: a pointer-sized type (a class or interface) gets
            // truncated to 32 bits and survives until the IR verifier
            // happens to catch the type mismatch at a function signature
            // (#3540), or — the unlucky case in #495 — does not, and is
            // later stored as a truncated half-pointer in a struct field
            // (SIGSEGV) or used in a load (SIGABRT). The spec asks for a
            // diagnostic naming the type (§2.8.3 rule 3, "a build whose
            // closure contains disagreeing descriptions … is ill-formed,
            // with a diagnostic naming the artifacts and recommending
            // recompilation of the dependents"); emit one and return the
            // i32 placeholder so the caller still has a Type* to thread
            // through (hasErrors() rejects the module at the end of
            // emission per #628).
            //
            // ⚠ SCOPE: the `.`-contains predicate is the §11.6 QUALIFIED
            // PATH discriminator the issue's analysis recommends
            // (verbatim: "A narrower predicate — 'reject when the
            // unresolved name contains a `.`' — would work for the
            // nested-type family specifically"). An `importedName`-empty
            // shape (a bare unbound type parameter `T` in an
            // UNSPECIALIZED generic-method TEMPLATE body) is intentionally
            // NOT diagnosed: the template body is emitted as "dead" IR per
            // #567 and the i32 placeholder is what keeps the template's
            // signatures consistent with its per-instantiation
            // specializations (#460). An `importedName`-set but
            // dot-free shape (a CROSS-MODULE generic-method type parameter
            // like `T`, where the loader correctly carries the parameter
            // NAME across the artifact boundary but cannot bind it to a
            // local declaration — the substitution is applied at the
            // consumer's monomorphization site, not here) is also NOT
            // diagnosed: diagnosing it would reject every cross-module
            // program that uses a generic method on a generic class.
            // // per §2.8.3 rule 3
            if (!rt->importedName.empty() &&
                rt->importedName.find('.') != std::string_view::npos &&
                !ctx_.diagnostics().hasErrors()) {
                SourceLocation at = rt->astNode ? rt->astNode->span.begin
                                                : SourceLocation{};
                std::string name(!rt->headName().empty()
                                     ? std::string(rt->headName())
                                     : std::string("<unnamed>"));
                ctx_.diagnostics().report(
                    Severity::Error, at,
                    std::string("cannot lower imported Named type `") + name +
                        "`: the type is unresolved at codegen (no "
                        "declaration matched any builtin arm — DateTime/"
                        "TimeSpan/Guid/Range/Span/heap-pointer builtins/"
                        "exception vtable — and no generic substitution "
                        "applied); §2.8.3 rule 3 requires the artifact "
                        "interface to carry enough to reconstruct every "
                        "public signature. Without that, i32 is the silent "
                        "fallback and a pointer-sized type is truncated to "
                        "32 bits (#3577, residue of #3540).");
            }
            return i32();  // value-kind named — int
        case sema::TypeRef::Kind::InlineArray: {
            // §15.1: [N]T is an LLVM fixed-size array [N x elemType].
            // N is an IntLiteralExpr, or a folded compile-time size (#734:
            // arraySizeValue, set by sema for a member-`const` size); element
            // type recurses.
            // fix #3183 (§15.1 / §2.8.3 rule 4, v0.254): admit a size that
            // survived as a FOLDED VALUE with no AST node behind it — which is
            // every IMPORTED inline array, whose `arraySizeValue` the loader now
            // restores from the metadata. The old guard demanded the `ast::Expr`
            // and so gave up before ever reaching the `arraySizeValue` arm
            // fifteen lines below, returning the bare `i32` placeholder: a
            // 4-byte layout against the exporter's `N * sizeof(elem)`, measured
            // as an ASan stack-buffer-overflow on construction.
            //
            // These two lines are now EXACTLY
            // `sema::serializationComponentTypeIsEmittable`'s condition
            // (SymbolCollection.cpp), whose comment already claimed to state
            // that condition "once, here, so the two cannot drift". They HAD
            // drifted — harmlessly, only because no importer ever set
            // `arraySizeValue` — and #3183 is what makes the difference
            // observable. A size that is neither an AST expression nor a folded
            // value still falls to the placeholder, which is what keeps an
            // unfoldable size failing loudly rather than silently.
            if (!rt->arrayElement) return i32();
            if (!rt->arraySize && rt->arraySizeValue < 0) return i32();
            // fix #3528 (§4.7 / §15.1, v0.286): `unitStorageTy`, the ONE
            // spelling of "the storage form of a lowered type" (#3527). §15.1's
            // element-type rule reads over the total, disjoint §3.2
            // classification ("value type" / "class type" / "runtime-managed
            // reference type") with no unit-type carve-out — unlike the
            // explicit `[N][M]T` multi-dimensional exclusion beside it — and
            // §4.7/§3.2 both classify the unit type as a value type, so `[N]()`
            // is an admitted element shape, not a rejection. `lowerTypeRef`
            // answers `void` for exactly the unit type's `Kind::Unit` arm, and
            // `void` is not a legal LLVM array ELEMENT type any more than it is
            // a legal alloca/parameter/struct-member type: `ArrayType::get`
            // does not itself validate this in a release (assertions-off)
            // LLVM, so the trap surfaced one level up, inside
            // `DataLayout::getPrefTypeAlign` at the array's OWN alloca —
            // `entryAlloca` already substitutes its TOP-level type, but a `[3
            // x void]` array is not void at the top level, so that guard never
            // saw it (measured rc=132, SIGILL, no diagnostic, for both a local
            // `var a: [3]() := [(), (), ()];` and a parameter `(a: [2]())`).
            // Every existing storage substitution in this family tests the
            // TOP-level lowered type; this is the first one applied one level
            // down, at the ELEMENT — the same fix `unitStorageTy` exists to
            // keep from being re-derived at a ninth site.
            llvm::Type* elem = unitStorageTy(lowerTypeRef(rt->arrayElement));
            // fix #2936: decode the lexeme with the ONE literal parser. This
            // was `std::stoull(text)` — base 10 — so `[0x5]int` became a
            // 0-element array and `[5_0]int` a 5-element one, SILENTLY. Note
            // this arm is tried BEFORE the folded `arraySizeValue` below, so a
            // bare literal never reached sema's (correct) fold: `[0x3 + 2]int`
            // was right while `[0x5]int` was wrong, which is what made the
            // split visible.
            if (auto* lit =
                    dynamic_cast<const ast::IntLiteralExpr*>(rt->arraySize))
                return llvm::ArrayType::get(
                    elem,
                    static_cast<std::uint64_t>(parseIntLiteral(lit->text)));
            if (rt->arraySizeValue >= 0)  // #734: folded member-const size
                return llvm::ArrayType::get(
                    elem, static_cast<uint64_t>(rt->arraySizeValue));
            return i32();
        }
        case sema::TypeRef::Kind::Function:
            // CG60: a function type `(T1, …) -> R` — stored as an opaque
            // pointer to the function body (a plain function pointer in
            // the non-capturing / single-representation case).
            return ptrTy();
        default:
            return i32();
    }
}


llvm::Type* Emitter::storageType(const sema::Symbol* m) {
    // fix #2533 (§24.1/§22.8/§2.8.2 clause 1, v0.236): an event's backing
    // storage is a runtime `EventSource<T>` (`AsyncEventSource<T>` for async)
    // referenced by an OPAQUE POINTER (§24.1), NOT the event's payload type.
    // The wire-carried MetaMember::type for an IMPORTED event is its `: T`
    // PAYLOAD type (e.g. `EvB2421` in the issue's repro), not the EventSource
    // storage type — so the imported-member-type fallback below would happily
    // pick that payload type up and LAY the struct out with the payload record
    // in the event slot, mis-aligning the importer's struct relative to the
    // exporter's. Concretely: the exporter's BaseWithEvt2421 struct is `{ ptr,
    // i8, ptr }` (vtable, disposed flag, EventSource ptr) at offset 16; the
    // importer's Derived2421 struct, with the event laid out as the payload
    // record `EvB2421`, became `{ ptr, i8, %EvB2421 }` (where %EvB2421 is a
    // `{ i32 }` payload record, alignment 4) — the EventSource landed at
    // offset 12 inside that struct. The exporter's `bz$<Base>$init` (called
    // via `emitImportedBaseFieldInit`) writes the EventSource at offset 16
    // per the exporter's struct, but the importer's itable accessor
    // (`bz$Derived2421$Ready$evtget`) reads at offset 12 per the importer's
    // struct — the byte read is not the EventSource, so `bz_event_add`
    // dereferences a junk pointer and SEGVs (#2533). This check has to come
    // BEFORE the imported-member-type fallback (and the fd/pd branches, which
    // are also decl-dependent) so EVERY event — local or imported, sync or
    // async, payload or unit — gets the same opaque pointer storage the
    // exporter uses. The `isEventStatic`/`isEventAsync` wire bits
    // (added by #2421) carry the sync/async classification that determines
    // whether the slot is initialized at all (`collectInstanceStorage` /
    // `emitOwnFieldInit` gate on `eventIsStatic`); the storage TYPE itself is
    // always `ptr` regardless of either bit.
    if (dynamic_cast<const sema::EventSymbol*>(m))
        return ptrTy();
    // §9.1: a record's positional field carries its type on the
    // originating Param, not a FieldDecl (its `decl` is the TypeDecl).
    // fix #2516: for an IMPORTED positional field the importer synthesizes a
    // null-type ast::Param so `fs->posParam != nullptr` distinguishes
    // positional from body fields at the sema layer (SymbolCollection.cpp /
    // ModuleLoader.cpp) — but the type itself is NOT recoverable from that
    // synthetic Param. Fall through to the importedMemberType side table so
    // the layout gets the real field type from `fieldTypes_`.
    if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m))
        if (fs->posParam && fs->posParam->type)
            // fix #3527 (§4.7 / §9.1, v0.283): `unitStorageTy`, like the body-
            // field and auto-property arms below. #2827 added the `()`→one-byte
            // substitution to those two and not to this one, so a §9.1
            // POSITIONAL field of unit type stayed `void` and `recordStruct`
            // laid the record out with a `void` MEMBER. `setupBoxDescriptors`
            // mints a ToString thunk for every record in the module and that
            // thunk LOADS the record value, so merely DECLARING `record R(A:
            // int, U: ());` — with no value of it anywhere — killed `bezelc`
            // with SIGILL and no diagnostic (measured rc=132 on ef339fa).
            return unitStorageTy(lowerType(fs->posParam->type.get()));
    // §2.5 / §22.8 (issue #328 FINDING [7]): an IMPORTED instance field has no
    // ast::FieldDecl — its resolved type rides the importedMemberType side table
    // (ModuleLoader). Without consulting it, an imported field of a non-int type
    // (string/class/long/double) would fall through to the i32 default below,
    // giving the imported instance the WRONG layout (size + every field offset
    // after it), so a cross-module `new` + field read would load garbage. The
    // field type is needed for the instance layout the constructor and field
    // accesses depend on; this is the layout half of the FINDING [7] fix.
    if (auto* mem = dynamic_cast<const sema::MemberSymbol*>(m))
        if (mem->decl == nullptr)
            if (sema::TypeRef* tr = ctx_.importedMemberType(mem))
                return lowerTypeRef(tr);
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl)) {
        // fix #2827 (§14.2 / §4.7, v0.282): a field whose declared type is
        // the NON-nullable unit type (`()`) must substitute the §4.7 one-byte
        // placeholder (`unitPlaceholderTy`) — `lowerType`'s `Kind::Unit` arm
        // returns `void`, and `void` is unsized, so a struct field typed
        // `void` produces a GEP into an unsized type and the IR verifier
        // aborts. Concretely:
        //   `public class Holder<T> { public v: T; }
        //    using var h := new Holder<()>();`
        // emitted `getelementptr … %"class.Holder$spec0", ptr %1, i32 0, i32
        // 0` on a `void`-typed field 0 — MEASURED on d34c8b4. The substitution
        // is the SAME `unitPlaceholderTy()` swap the §4.7 local-decl path
        // (`lowerLocalDecl`, #1619) and the parameter-signature builders
        // (#1619 / #3105 / #4493) already make — each unit field occupies
        // one inert byte, and §4.7's "one value" property means no byte
        // pattern distinguishes it from another, so the placeholder is
        // faithful. A `()?` target never reaches here (`isNullableValueTy`
        // claims `()?` first and returns the tagged `{i8, i8}` optional — a
        // real first-class type, no substitution needed).
        // fix #3527: asked through `unitStorageTy` rather than spelled inline,
        // so this arm and the two beside it cannot drift again.
        return unitStorageTy(lowerType(fd->type.get()));
    }
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl)) {
        // Same #2827 substitution as the field arm above — an auto-property
        // with `()?`-shaped backing storage is reduced by `lowerType` to its
        // declared type's LLVM type, which is `void` for `()`. Gated
        // identically.
        // fix #3527: same one spelling as the two arms above.
        return unitStorageTy(lowerType(pd->type.get()));
    }
    return i32();
}


// fix #2889 (§14.1 "Generic declarations" / §12 "Inheritance and Dispatch" /
// §22.8 "Field initializers", v0.240). See the declaration comment in
// LlvmBackend_impl.hpp.
//
// The layout/substitution machinery keys type-parameter bindings by PARAMETER
// NAME in one flat map. When `t` is a GENERIC class, every site that installs a
// substitution for it binds `t`'s OWN parameter names only (`lowerNew` from the
// `new` site's type arguments, method emission from the receiver's). Storage
// INHERITED from a generic ancestor is declared in terms of the ANCESTOR's
// parameter names, so unless `t` happens to spell the base's argument with the
// same name (`class D<T> : B<T>` — the one shape that worked), no binding named
// it and `storageType` fell to its i32 placeholder for every inherited
// parameter-typed field.
//
// This composes the missing half: for each generic ancestor on `t`'s class base
// chain, ask `computeAncestorGenericSubst` (the shared #2840 helper #2860/#2873/
// #2882 already use, which resolves each level's base arguments through the
// level below and refuses rather than guessing) for that ancestor's own
// parameter bindings, and report the ones the active substitution does not
// already carry.
//
// MOST-DERIVED WINS. The chain is walked outward-in and a name already bound —
// by the active substitution, or by a nearer ancestor — is never overwritten.
// That keeps `t`'s own parameters meaning `t`'s own type arguments for `t`'s own
// fields. It also means a base that REUSES a derived parameter's spelling for a
// different argument (`class D<T> : B<string>` where `B`'s parameter is also
// named `T`) keeps today's behaviour: one flat name-keyed map cannot represent
// two bindings of `T` at once. That is a pre-existing limitation of the
// substitution representation, not of this composition — filed separately rather
// than papered over here.
bool Emitter::ancestorGenericSubstAdditions(
        const sema::TypeSymbol* t,
        std::unordered_map<std::string_view, llvm::Type*>& addSubst,
        std::unordered_map<std::string_view, const sema::TypeRef*>& addSema) {
    if (!t || t->genericParams.empty() || genericSubst_.empty()) return false;
    sema::TypeRef synth;
    synth.kind = sema::TypeRef::Kind::Named;
    synth.declaration = const_cast<sema::TypeSymbol*>(t);
    for (const sema::TypeSymbol* anc = baseClassOf(t); anc;
         anc = baseClassOf(anc)) {
        if (anc->genericParams.empty()) continue;
        std::unordered_map<std::string_view, llvm::Type*> ancSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> ancSema;
        if (!computeAncestorGenericSubst(&synth, anc, ancSubst, ancSema))
            continue;
        for (const auto& [nm, ty] : ancSubst) {
            if (genericSubst_.count(nm) || addSubst.count(nm)) continue;
            addSubst[nm] = ty;
            auto sit = ancSema.find(nm);
            if (sit != ancSema.end()) addSema[nm] = sit->second;
        }
    }
    return !addSubst.empty();
}


// fix #2897 (§14.1 "Generic declarations" / §12 "Inheritance and Dispatch" /
// §22.8 "Field initializers", v0.240). See the declaration comment in
// LlvmBackend_impl.hpp.
std::vector<const sema::TypeSymbol*> Emitter::instanceStorageOwners(
        const sema::TypeSymbol* t, std::size_t n) {
    // Default every slot to `t` itself, so a slot no level claims (which the
    // prefix accounting below cannot produce, but a future change might) is
    // resolved exactly as today rather than under some ancestor's map.
    std::vector<const sema::TypeSymbol*> owners(n, t);
    std::vector<const sema::TypeSymbol*> chain;
    for (const sema::TypeSymbol* c = t; c; c = baseClassOf(c))
        chain.push_back(c);
    // Root-first: `collectInstanceStorage(L)` is L's ancestors' storage followed
    // by L's own (§ prefix layout), so the slots it adds beyond the level above
    // are exactly the ones L declares.
    std::size_t prev = 0;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        std::vector<const sema::Symbol*> s;
        collectInstanceStorage(*it, s);
        for (std::size_t i = prev; i < s.size() && i < n; ++i) owners[i] = *it;
        prev = s.size();
    }
    return owners;
}


// fix #2897 (§14.1 / §12 / §22.8, v0.240). See the declaration comment in
// LlvmBackend_impl.hpp.
std::vector<llvm::Type*> Emitter::instanceStorageTypes(
        const sema::TypeSymbol* t) {
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(t, storage);
    std::vector<llvm::Type*> tys(storage.size(), nullptr);
    if (storage.empty()) return tys;
    if (genericSubst_.empty()) {
        for (std::size_t i = 0; i < storage.size(); ++i)
            tys[i] = storageType(storage[i]);
        return tys;
    }
    const std::vector<const sema::TypeSymbol*> owners =
        instanceStorageOwners(t, storage.size());

    // Phase 1: each generic ANCESTOR's OWN parameter bindings, resolved while the
    // ACTIVE substitution is still installed — `computeAncestorGenericSubst`
    // seeds a bare synthesized receiver out of it (#2889) and re-derives every
    // level POSITIONALLY from the base clause, which is why a shared parameter
    // spelling cannot confuse it. An ancestor it cannot resolve is left out and
    // keeps the flat map, exactly as today.
    std::vector<const sema::TypeSymbol*> ancs;
    std::vector<std::unordered_map<std::string_view, llvm::Type*>> ancSubsts;
    std::vector<std::unordered_map<std::string_view, const sema::TypeRef*>>
        ancSemas;
    sema::TypeRef synth;
    synth.kind = sema::TypeRef::Kind::Named;
    synth.declaration = const_cast<sema::TypeSymbol*>(t);
    for (const sema::TypeSymbol* o : owners) {
        if (!o || o == t || o->genericParams.empty()) continue;
        bool seen = false;
        for (const sema::TypeSymbol* a : ancs) if (a == o) { seen = true; break; }
        if (seen) continue;
        std::unordered_map<std::string_view, llvm::Type*> s;
        std::unordered_map<std::string_view, const sema::TypeRef*> sm;
        if (!computeAncestorGenericSubst(&synth, o, s, sm)) continue;
        ancs.push_back(o);
        ancSubsts.push_back(std::move(s));
        ancSemas.push_back(std::move(sm));
    }
    if (ancs.empty()) {
        for (std::size_t i = 0; i < storage.size(); ++i)
            tys[i] = storageType(storage[i]);
        return tys;
    }

    // Phase 2: each member under its declaring class's substitution. `installed`
    // tracks which map is currently in place (ancs.size() == the caller's own),
    // so the maps are copied only when the declaring class actually changes —
    // storage is grouped by level, so that is once per level.
    auto savedSubst = genericSubst_;
    auto savedSema = genericSubstSemaTy_;
    std::size_t installed = ancs.size();
    for (std::size_t i = 0; i < storage.size(); ++i) {
        std::size_t ai = ancs.size();
        for (std::size_t k = 0; k < ancs.size(); ++k)
            if (ancs[k] == owners[i]) { ai = k; break; }
        if (ai != installed) {
            if (ai == ancs.size()) {
                genericSubst_ = savedSubst;
                genericSubstSemaTy_ = savedSema;
            } else {
                genericSubst_ = ancSubsts[ai];
                genericSubstSemaTy_ = ancSemas[ai];
            }
            installed = ai;
        }
        tys[i] = storageType(storage[i]);
    }
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSema);
    return tys;
}


// fix #2896 (§14.1 "Generic declarations" / §12 "Inheritance and Dispatch" /
// §22.8 "Field initializers", v0.240). See the declaration comment in
// LlvmBackend_impl.hpp.
//
// The active substitution is composed with whatever `t` inherits from a generic
// ancestor before the comparison (`mergeAncestorGenericSubst`, #2889) for the
// same reason `emitBaseDisposeChain` does it: without that, a generic derived
// class over a generic base measures the SAME erased placeholder on both sides
// for the inherited slots and the test wrongly concludes "identical".
bool Emitter::instanceLayoutDiffersFromErasedTemplate(const sema::TypeSymbol* t) {
    if (!t || t->genericParams.empty() || genericSubst_.empty()) return false;
    auto savedSubst = genericSubst_;
    auto savedSema = genericSubstSemaTy_;
    mergeAncestorGenericSubst(t);
    std::vector<llvm::Type*> specLayout = instanceStorageTypes(t);
    genericSubst_.clear();
    genericSubstSemaTy_.clear();
    std::vector<llvm::Type*> tmplLayout = instanceStorageTypes(t);
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSema);
    return tmplLayout != specLayout;
}


llvm::StructType* Emitter::classStruct(const sema::TypeSymbol* t) {
    // CG29: a record has no vtable header; route to recordStruct so every
    // classStruct caller (field access, etc.) gets the right layout.
    if (t->kind == ast::TypeDeclKind::Record) return recordStruct(t);
    // fix #4114 (§35.1/§35.8, v0.295): a choice's instance storage is its
    // cases' tagged payloads (choiceStruct's `{i32 tag, case0, case1, …}`),
    // never a class's `{vtable*, disposed-flag, fields…}` shape — a choice
    // has no instance fields at all (§35.1: "Choices cannot have …
    // instance fields, block-form or storage-backed properties"). Route
    // here exactly like Record so that ANY caller reaching classStruct with
    // a Choice TypeSymbol (setupBoxDescriptors's box-vtable Pass C, added
    // for choices by fix #3900, is the one that crashed) gets the real
    // layout instead of building and caching a bogus zero-field class
    // struct. That bogus struct was cached into the SHARED `structs_` map
    // keyed on `t` — the same map choiceStruct() itself reads — so a later
    // choiceStruct(t) call (e.g. boxToStringThunk's default-ToString
    // synthesis via emitChoiceToString) retrieved the poisoned `{ptr, i8}`
    // entry instead of the real tag+payload layout. emitChoiceToString's
    // multi-index `CreateExtractValue(val, {1+ci, pi})` then indexed a
    // second level into the `i8` field (not a struct), which LLVM's
    // (non-assertions) release build resolves to a null-typed extract —
    // SIGSEGV inside llvm::Value::setName on construction, not a bounds
    // check we could catch here.
    if (t->kind == ast::TypeDeclKind::Choice) return choiceStruct(t);

    // fix #2889 (§14.1 "Generic declarations" / §12 "Inheritance and
    // Dispatch" / §22.8 "Field initializers", v0.240) — the ALLOCATION face of
    // the placeholder-layout family (#2840 access, #2860 imported field init,
    // #2873 disposer + local field init, #2882 implicit base ctor). See
    // `ancestorGenericSubstAdditions` for the full rationale.
    //
    // The specialized path below substitutes field types by PARAMETER NAME, and
    // the active substitution binds only `t`'s OWN parameter names. For a
    // GENERIC derived class over a generic base (`class D<U> : B<U>`, or
    // `class D<U> : B<string>`) the inherited storage is declared in terms of
    // `B`'s parameter `T`, which no active binding names — so `storageType` fell
    // to its i32 placeholder and `class.D$specN` was built at the ERASED size.
    // `lowerNew` sizes its calloc from exactly this struct, so `new D<string>()`
    // allocated the T→i32 object while every reader (correct since #2840) GEP'd
    // the T→ptr offsets: a heap-buffer-overflow on every inherited field past
    // the first widened one, on ordinary single-module code (#2889).
    //
    // The non-generic-derived walk further down already composes exactly these
    // bindings, but it is gated on `t->genericParams.empty()` and so never ran
    // for a generic derived class.
    //
    // Bounded re-entry: the merged map is installed and `classStruct(t)` is
    // re-entered ONCE. On the re-entry the same helper proves the same bindings,
    // all of which are now present, so nothing new is added and control falls
    // through to the specialized path.
    if (!genericSubst_.empty() && !t->genericParams.empty()) {
        std::unordered_map<std::string_view, llvm::Type*> addSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> addSema;
        if (ancestorGenericSubstAdditions(t, addSubst, addSema)) {
            auto savedSubst = genericSubst_;
            auto savedSemaTy = genericSubstSemaTy_;
            for (const auto& [nm, ty] : addSubst) genericSubst_[nm] = ty;
            for (const auto& [nm, tr] : addSema) genericSubstSemaTy_[nm] = tr;
            llvm::StructType* result = classStruct(t);
            genericSubst_ = std::move(savedSubst);
            genericSubstSemaTy_ = std::move(savedSemaTy);
            return result;
        }
    }

    // CG-generic: if we have an active substitution, compute the concrete
    // field types first and look them up in the specialization cache.
    if (!genericSubst_.empty()) {
        // Peek at what field types we'd get with the substitution.
        std::vector<const sema::Symbol*> storage;
        collectInstanceStorage(t, storage);
        std::vector<llvm::Type*> specFieldTys;
        specFieldTys.push_back(ptrTy());            // [0] vtable pointer
        // #595 / §31.4: a class that directly inherits a built-in exception
        // mirrors the FULL 40-byte built-in header (Message@8, Inner@16,
        // CauseChain@24, ParamName@32) — see classStruct. The disposed flag
        // moves to [5] in that case.
        if (hasBuiltinExceptionBase(t)) {
            specFieldTys.push_back(ptrTy());        // [1] inherited Message
            specFieldTys.push_back(ptrTy());        // [2] InnerException
            specFieldTys.push_back(ptrTy());        // [3] CauseChain
            specFieldTys.push_back(ptrTy());        // [4] ParamName
        }
        specFieldTys.push_back(disposedFlagTy());   // [1 or 5] disposed run-once flag
        // fix #2897 (§14.1 / §12 / §22.8, v0.240): resolve each slot under the
        // substitution of the class that DECLARES it. The flat name-keyed map
        // cannot hold two bindings of one spelling at once, so a base that reuses
        // a derived parameter's name for a DIFFERENT argument
        // (`class Coll<T> : Base<string>`, `Base`'s parameter also `T`) laid the
        // inherited slot out at the DERIVED argument's width while `fieldAddr`'s
        // inherited branch (correct since #2840) GEP'd the base's — an 8-byte
        // store into a 4-byte slot, corrupting the field after it.
        for (llvm::Type* fty : instanceStorageTypes(t))
            specFieldTys.push_back(fty);
        // Cache lookup: specializedStructs_[t][specFieldTys] → existing
        auto& specMap = specializedStructs_[t];
        auto sit = specMap.find(specFieldTys);
        if (sit != specMap.end()) return sit->second;
        // Create new specialized struct.
        std::string specName = std::string("class.") + std::string(t->name)
            + "$spec" + std::to_string(specMap.size());
        auto* specSt = llvm::StructType::create(llvm_, specName);
        specSt->setBody(specFieldTys);
        specMap[specFieldTys] = specSt;
        // Register field indices for this specialization.  Data fields start
        // after the vtable, the optional message-ptr, and the disposed flag.
        unsigned sidx = disposedFlagIndex(t) + 1;
        for (const sema::Symbol* m : storage)
            fieldIndex_[m] = sidx++;
        return specSt;
    }

    // §14 / #617 / #1101-B: a non-generic class `D` that inherits a concrete
    // generic instantiation `Base<string>` — DIRECTLY or through intermediate
    // non-generic links (`Child: Mid: Box<string>`) — has inherited generic-T
    // fields whose LLVM types depend on the ancestor's concrete type args
    // (e.g. `_v: T` must be ptr for T=string, not the i32 placeholder that
    // falls out of an unresolved T). Walk the FULL class base chain, composing
    // each generic ancestor's param→concrete-arg bindings into one substitution
    // (resolving a base's args through the bindings accumulated at lower levels,
    // so `Mid<U>: Box<U>` links compose), then run through the specialised-struct
    // path so storageType uses the right concrete type for every inherited field.
    // This also gives `D` a proper per-layout entry in specializedStructs_ (so
    // repeat calls with empty genericSubst_ find the correct specialization via
    // the specFieldTys cache key, not the unspecialized fallback below).
    // (The prior code only handled a single IMMEDIATE generic base, so a
    // grandchild `Child: Mid: Box<string>` kept the i32 placeholder for the
    // inherited field and read it from the wrong slot — #1101-B.)
    if (t->genericParams.empty()) {
        std::unordered_map<std::string_view, llvm::Type*> tmpSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> tmpSema;
        // curArgs: the current level's generic-param-name → concrete TypeRef.
        // `t` is non-generic so it starts empty; each base level resolves its
        // own args through it (composition).
        std::unordered_map<std::string_view, const sema::TypeRef*> curArgs;
        for (const sema::TypeSymbol* cur = t; cur;) {
            const sema::TypeSymbol* base = baseClassOf(cur);
            if (!base) break;
            const sema::TypeRef* baseRef = nullptr;
            for (std::size_t bi = 0; bi < cur->baseTypes.size(); ++bi) {
                if (cur->baseTypes[bi] == base) {
                    baseRef = (bi < cur->baseTypeRefs.size())
                                  ? cur->baseTypeRefs[bi] : nullptr;
                    break;
                }
            }
            std::unordered_map<std::string_view, const sema::TypeRef*> nextArgs;
            if (!base->genericParams.empty()) {
                // fix #2840 (§14.1 / §2.5, v0.240): an IMPORTED generic base has
                // NO ast::TypeDecl — ModuleLoader::reconstructTypeSymbol builds a
                // sema::TypeSymbol from the metadata and never sets `decl` (there
                // is no AST node in the importer) — but it DOES populate
                // `genericParams` positionally from the metadata
                // (ModuleLoader.cpp:339). Reading the parameter NAMES off
                // `base->decl` therefore made this walk `break` for every
                // cross-module base, dropping `D : Base<string>` to the
                // UNSPECIALIZED (placeholder) layout in which the inherited
                // `v: T` is the i32 fallback rather than ptr — malformed IR on any
                // read of the inherited field, and a silently truncating store on
                // any write. Take the names from the AST decl when present, else
                // from the imported TypeSymbol — the same `td ? td->genericParams
                // : ts->genericParams` fallback already written for the call path
                // (LlvmBackend_Calls2.cpp, §14.1/#783) and the static-access path
                // (§22.12/#684), and the reason `computeAncestorGenericSubst`
                // (which binds T→ptr for exactly this receiver) did not have the
                // gap. Both sequences are positional against
                // `baseRef->genericArgs`, so the arity check below is unchanged.
                // fix #3143: that fallback is now `sema::genericParamNames`,
                // the one shared walk, not this site's own copy of it.
                const std::vector<std::string_view> gpNames =
                    sema::genericParamNames(base);
                if (!baseRef || gpNames.empty() ||
                    gpNames.size() != baseRef->genericArgs.size())
                    break;  // unresolvable — fall through to unspecialized layout
                for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                    const sema::TypeRef* arg = baseRef->genericArgs[gi];
                    // Resolve a bare generic-param ref (`T`, no declaration)
                    // through the lower level's bindings; concrete args pass
                    // through unchanged.
                    if (arg && arg->kind == sema::TypeRef::Kind::Named &&
                        !arg->declaration && arg->astNode && arg->astNode->name &&
                        arg->astNode->name->parts.size() == 1) {
                        auto it = curArgs.find(arg->headName());
                        if (it != curArgs.end()) arg = it->second;
                    }
                    std::string_view pname = gpNames[gi];
                    nextArgs[pname] = arg;
                    // Record the layout binding (keep the lowest/most-derived
                    // binding on a param-name collision — it is the concrete one).
                    if (arg && tmpSema.find(pname) == tmpSema.end()) {
                        tmpSubst[pname] = lowerTypeRef(arg);
                        tmpSema[pname] = arg;
                    }
                }
            }
            cur = base;
            curArgs = std::move(nextArgs);
        }
        if (!tmpSubst.empty()) {
            auto savedSubst = genericSubst_;
            auto savedSemaTy = genericSubstSemaTy_;
            genericSubst_ = std::move(tmpSubst);
            genericSubstSemaTy_ = std::move(tmpSema);
            llvm::StructType* result = classStruct(t);  // re-enter via spec path
            genericSubst_ = std::move(savedSubst);
            genericSubstSemaTy_ = std::move(savedSemaTy);
            return result;
        }
    }

    auto it = structs_.find(t);
    if (it != structs_.end()) return it->second;
    auto* st = llvm::StructType::create(
        llvm_, std::string("class.") + std::string(t->name));
    structs_[t] = st;  // record before filling (supports self-reference)
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(t, storage);
    std::vector<llvm::Type*> fieldTys;
    // CG17: slot 0 is the hidden vtable pointer.
    fieldTys.push_back(ptrTy());
    // #595 / §12 / §31.4: a user class that directly inherits a built-in
    // exception (e.g. `class ErrA: Exception`) needs to share the FULL built-in
    // exception object layout so the base-Exception member reads/writes hit the
    // same byte offsets regardless of whether `e` is typed as the user class or
    // as the built-in base. Built-in exceptions are a uniform kExcObjSize
    // (40-byte) block: [0]=vtable, [8]=Message, [16]=InnerException(TIE),
    // [24]=CauseChain, [32]=ParamName. Mirror all four data slots BEFORE the
    // disposed flag, giving: [vtable(0), message(1), inner(2), causechain(3),
    // paramname(4), disposed(5), own_fields…]. disposedFlagIndex() returns 5
    // for such classes so the disposer and disposal checks GEP the right slot.
    // (Before v0.163 only the message slot was mirrored; `base(msg, cause)`
    // then wrote CauseChain@24 into the disposed flag / past the block, and
    // `e.CauseChain` on a user instance read out of bounds — the exception-
    // surface merge-gate heap-overflow finding.)
    if (hasBuiltinExceptionBase(t)) {
        fieldTys.push_back(ptrTy());          // [1] inherited Message @8
        fieldTys.push_back(ptrTy());          // [2] InnerException @16 (unused)
        fieldTys.push_back(ptrTy());          // [3] CauseChain @24
        fieldTys.push_back(ptrTy());          // [4] ParamName @32
    }
    // §28.2 / §28.6: the per-object i8 "disposed" run-once flag.  It sits at a
    // fixed index in every class (1 normally, 2 for builtin-exception-derived
    // classes — see disposedFlagIndex), calloc-zeroed to 0 = not-disposed.
    fieldTys.push_back(disposedFlagTy());
    unsigned idx = disposedFlagIndex(t) + 1;
    for (const sema::Symbol* m : storage) {
        fieldTys.push_back(storageType(m));
        fieldIndex_[m] = idx++;
    }
    st->setBody(fieldTys);
    return st;
}


// §28.2 / §28.6: the LLVM type of the per-object "disposed" run-once flag — a
// single byte right after the vtable pointer in every class struct. calloc
// zeroes it (0 = not yet disposed); the generated Dispose sets it to 1 on first
// disposal.
llvm::Type* Emitter::disposedFlagTy() { return llvm::Type::getInt8Ty(llvm_); }


// §28.2 / §28.6 / #595: the struct index of a class's disposed flag.
//
// For ordinary classes: slot 1 (right after the vtable pointer, before all data
// fields).  Uniform across the hierarchy and generic specializations, so the
// shared generated Dispose accesses it correctly.
//
// For user classes that directly inherit a built-in exception type (§12 /
// §31.4): slot 5 — slots 1..4 are occupied by the inherited Message /
// InnerException / CauseChain / ParamName pointers that mirror the built-in
// exception's uniform 40-byte layout (byte offsets 8/16/24/32). Keeping the
// full header ensures every base-Exception member read/write hits the same
// byte offset whether the object is typed as the user class or as the
// built-in Exception base type.
unsigned Emitter::disposedFlagIndex(const sema::TypeSymbol* t) {
    if (t && hasBuiltinExceptionBase(t)) return 5u;
    return 1u;
}


llvm::StructType* Emitter::recordStruct(const sema::TypeSymbol* t) {
    // §9.1 / §9.2: storage = non-static FieldSymbols (positional + body
    // fields) and body auto-properties (PropertySymbol), in declaration order.
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(t, storage);

    // CG-generic (issue #460): a generic RECORD must specialize its T-typed
    // field layout per instantiation, exactly as classStruct does — otherwise a
    // `record Box<T>(value: T)` instantiated as `Box<string>` lays `value` out
    // as the unspecialized i32 generic placeholder while field access GEPs a
    // `%bz.string` against an i32 base (malformed IR). When a substitution is
    // active, compute the concrete field types under it and cache a SEPARATE
    // specialized struct per distinct field-type vector (specializedStructs_),
    // so Box<int> (i32 field) and Box<string> (ptr field) get distinct LLVM
    // struct types and each access GEPs against the matching layout. Records
    // carry no vtable/disposed-flag header (§9 — a record is a value type;
    // §9.7 — no disposer unless borrow-bearing), so fields start at index 0,
    // unlike classStruct which reserves indices 0/1 for the vtable + flag.
    //
    // #847: only a record that is ITSELF generic can have a substitution-
    // dependent layout. A non-generic record (e.g. `record Point(X:int,Y:int)`)
    // used as a generic type ARGUMENT (`Identity<Point>`, `Box<Point>`) has a
    // fixed `[i32,i32]` layout regardless of the active substitution — minting a
    // distinct `%record.Point$spec0` here makes it differ from the canonical
    // `%record.Point` that the call-site argument/field already uses, so a
    // `return v` (param passthrough) or by-value field read fails IR
    // verification (`%record.Point` vs `%record.Point$spec0`). Mirror the
    // `t->genericParams.empty()` gate that classStruct uses for the same reason.
    if (!genericSubst_.empty() && !t->genericParams.empty()) {
        std::vector<llvm::Type*> specFieldTys;
        specFieldTys.reserve(storage.size());
        for (const sema::Symbol* m : storage)
            specFieldTys.push_back(storageType(m));
        auto& specMap = specializedStructs_[t];
        auto sit = specMap.find(specFieldTys);
        if (sit != specMap.end()) {
            // Re-register field indices for this specialization so a subsequent
            // field access under the same subst GEPs the correct slot.
            unsigned sidx = 0;
            for (const sema::Symbol* m : storage) fieldIndex_[m] = sidx++;
            return sit->second;
        }
        std::string specName = std::string("record.") + std::string(t->name)
            + "$spec" + std::to_string(specMap.size());
        auto* specSt = llvm::StructType::create(llvm_, specName);
        specSt->setBody(specFieldTys);
        specMap[specFieldTys] = specSt;
        unsigned sidx = 0;
        for (const sema::Symbol* m : storage) fieldIndex_[m] = sidx++;
        return specSt;
    }

    auto it = structs_.find(t);
    if (it != structs_.end()) return it->second;
    auto* st = llvm::StructType::create(
        llvm_, std::string("record.") + std::string(t->name));
    structs_[t] = st;  // before filling, to support self-reference
    std::vector<llvm::Type*> fieldTys;
    unsigned idx = 0;
    for (const sema::Symbol* m : storage) {
        fieldTys.push_back(storageType(m));
        fieldIndex_[m] = idx++;
    }
    st->setBody(fieldTys);
    return st;
}


// §35 (v0.200, #2045): choice cases in declaration order. The case's index
// in this vector is its runtime tag value (stored at field 0 of the choice
// struct, read by the match-side tag comparison). The `ChoiceCaseSymbol`s
// live alongside other members of the choice TypeSymbol (§35.1, collected
// in SymbolCollection.cpp). Defined here (next to choiceStruct) since the
// layout and the case-index contract are inseparable.
// fix #3144 (review nit N2): FORWARDS to `sema::choiceCasesOf`. This was a
// character-for-character copy of that walk, and the two agreeing IS the
// tag-value contract — case index i is the runtime tag, so a divergence would
// not be a slow path, it would be sema and codegen disagreeing about what a
// tag means. `sema/Symbol.hpp` even documented the duplication ("the identical
// walk on the codegen side") rather than removing it.
//
// That is this programme's own headline defect (#3152): one question answered
// in two places, drifting silently. Keeping the member function is deliberate —
// ~20 call sites read better unqualified, and a forwarder cannot drift.
std::vector<const sema::ChoiceCaseSymbol*> Emitter::choiceCasesOf(
        const sema::TypeSymbol* choiceTy) {
    return sema::choiceCasesOf(choiceTy);
}

const sema::ChoiceCaseSymbol* Emitter::choiceCaseByName(
        const sema::TypeSymbol* choiceTy, std::string_view name,
        std::size_t* tagOut) {
    auto cases = choiceCasesOf(choiceTy);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (cases[i]->name == name) {
            if (tagOut) *tagOut = i;
            return cases[i];
        }
    }
    return nullptr;
}

// §35: a case's payload struct type — `{ type0, type1, ... }` for the
// case's payload fields (lowered via lowerType on each Param's AST type),
// or `{ i8 }` for a payload-free case. Every case occupies exactly one
// field in the choice struct so the active-case slot index is uniform
// (1 + caseIndex); a payload-free case still needs a slot for layout
// uniformity, so it gets the i8 placeholder. NB: this does NOT include
// the tag — the tag is added by choiceStruct at field 0.
llvm::StructType* Emitter::choiceCasePayloadStruct(
        const sema::TypeSymbol* /*choiceTy*/,
        const sema::ChoiceCaseSymbol* cc) {
    if (!cc || !cc->caseDecl || cc->caseDecl->payload.empty()) {
        return llvm::StructType::get(llvm_, {i8()}, /*isPacked=*/false);
    }
    std::vector<llvm::Type*> fieldTys;
    for (const ast::Param& pf : cc->caseDecl->payload) {
        if (!pf.type) {
            fieldTys.push_back(i32());  // defensive — unresolved → i32
            continue;
        }
        fieldTys.push_back(lowerType(pf.type.get()));
    }
    return llvm::StructType::get(llvm_, fieldTys, /*isPacked=*/false);
}

llvm::StructType* Emitter::choiceStruct(const sema::TypeSymbol* t) {
    // §35.13 / §14.1 (#2047): a GENERIC choice specializes its payload layout
    // per instantiation, exactly as recordStruct does (#460) — `Result<int,
    // string>` lays Error's payload as {ptr} (string), `Result<int, int>` as
    // {i32}. Without this the single `structs_` cache freezes the FIRST call's
    // layout (the i32 placeholder when the local is allocated in a non-generic
    // body), so a later store/load/dispose at the real instantiation reads the
    // wrong field type — an ICmp/GEP type mismatch. Under an active substitution
    // (installed by lowerTypeRef's concrete-choice branch or an inlined generic
    // body), build the concrete payload structs and cache a distinct specialized
    // struct per field-type vector. A non-generic choice keeps the single cache.
    if (!genericSubst_.empty() && !t->genericParams.empty()) {
        std::vector<llvm::Type*> specFieldTys;
        specFieldTys.push_back(i32());  // tag
        for (const sema::ChoiceCaseSymbol* cc : choiceCasesOf(t))
            specFieldTys.push_back(choiceCasePayloadStruct(t, cc));
        auto& specMap = specializedStructs_[t];
        auto sit = specMap.find(specFieldTys);
        if (sit != specMap.end()) return sit->second;
        std::string specName = std::string("choice.") + std::string(t->name)
            + "$spec" + std::to_string(specMap.size());
        auto* specSt = llvm::StructType::create(llvm_, specName);
        specSt->setBody(specFieldTys);
        specMap[specFieldTys] = specSt;
        return specSt;
    }

    auto it = structs_.find(t);
    if (it != structs_.end()) return it->second;
    auto* st = llvm::StructType::create(
        llvm_, std::string("choice.") + std::string(t->name));
    structs_[t] = st;  // before filling, to support self-reference

    // §35: `{ i32 tag, case0_payload, case1_payload, ... }`. Each case's
    // payload occupies field index `1 + caseIndex`. The tag (field 0) is
    // the case's declaration-order index.
    std::vector<llvm::Type*> fieldTys;
    fieldTys.push_back(i32());  // tag — §35.13 implementation-defined width
    auto cases = choiceCasesOf(t);
    for (const sema::ChoiceCaseSymbol* cc : cases) {
        fieldTys.push_back(choiceCasePayloadStruct(t, cc));
    }
    st->setBody(fieldTys);
    return st;
}


// §9 / §9.1 (#1759): recover the record TypeSymbol of the instance method
// currently being emitted, or null when the receiver is not a record (class /
// enum / extension), the method is static, or none is in flight. Mirrors the
// owner recovery lowerWith uses for `this with … end` (#575 / #628): the
// reverse-lookup of currentFn_ in methodFns_ handles ordinary bodies, and the
// currentSpecMethodSymbol_ fallback handles a specialized generic body that
// buildSpecializedMethodFn never registered in methodFns_. The
// currentCtorParentRecord_ fallback (fix #4056) handles record CONSTRUCTOR
// bodies, where currentFn_ is the ctor's function (which is never registered
// in methodFns_ — and has no MethodSymbol) and currentSpecMethodSymbol_ is
// null, but `this` is still a pointer to a record (CG29) that needs to be
// loaded into the record struct when it reaches a callee expecting the
// struct by value (§9.4, §22.3).
const sema::TypeSymbol* Emitter::currentReceiverRecordType() {
    if (!currentThis_) return nullptr;
    if (currentFn_) {
        for (const auto& [ms, fn] : methodFns_)
            if (fn == currentFn_ && ms->parent &&
                ms->parent->kind == ast::TypeDeclKind::Record)
                return ms->parent;
    }
    if (currentSpecMethodSymbol_ && currentSpecMethodSymbol_->parent &&
        currentSpecMethodSymbol_->parent->kind == ast::TypeDeclKind::Record)
        return currentSpecMethodSymbol_->parent;
    if (currentCtorParentRecord_ &&
        currentCtorParentRecord_->kind == ast::TypeDeclKind::Record)
        return currentCtorParentRecord_;
    return nullptr;
}


// §9 / §9.1 (#1759): a record has value semantics — `return this`,
// `slot := this`, and passing `this` by value all copy the struct. Inside a
// record method `this` lowers to a POINTER to the by-value receiver (CG29 /
// §9.2 / #794 — spilled so `this.field` reuses record field access), so a
// value-position use that reaches a struct-typed destination would otherwise
// propagate the raw 8-byte pointer (garbage into the slot; `ret ptr` where the
// struct is expected → IR verification failure). Load the struct exactly when
// `v` is that receiver pointer and `expectedTy` is precisely the current
// receiver record's struct layout — never for any other value, a class/enum
// receiver (`currentThis_` is the value / a non-record pointer), or a
// nullable-tagged {i8, T} / unrelated struct destination (whose type differs
// from recordStruct, so the equality guard rejects it). The receiver's own
// generic substitution is active while its body is emitted, so recordStruct
// here yields the same specialized layout `expectedTy` was computed from.
llvm::Value* Emitter::coerceThisRecordValue(llvm::Value* v,
                                            llvm::Type* expectedTy) {
    if (!v || v != currentThis_ || !v->getType()->isPointerTy() || !expectedTy)
        return v;
    if (const sema::TypeSymbol* rec = currentReceiverRecordType())
        if (expectedTy == recordStruct(rec))
            return builder_.CreateLoad(expectedTy, v);
    return v;
}


// issue #460 / #462 / §14.1: install the type-parameter substitution for a
// concrete generic-record instantiation (`Box<string>`), so a subsequent
// recordStruct specializes the field layout, and recordMemberTypeResolved maps
// each `T`-typed field to its concrete type. The OUTER substitution is saved and
// restored, so nested generics chain. Builds the map from `rt`'s genericArgs
// resolved under any outer subst (lowerTypeRef honors the active substitution),
// the identical construction lowerTypeRef's #460 record-annotation branch and
// fieldAddr already use. A no-op when `rt` is not a concrete generic-record
// instantiation whose arg count matches the declared param count.
Emitter::RecordSubstGuard::RecordSubstGuard(Emitter* em, const sema::TypeRef* rt)
    : e(em) {
    // §14.1 substitution install. Records are the original client; §35.4
    // (v0.200, #2047) reuses the identical mechanism for a generic CHOICE
    // instantiation (`Result<C, string>`) so its owning-disposal / layout sees
    // the specialized payload struct — the param↔arg binding below is
    // kind-agnostic (both carry `genericParams`).
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration ||
        (rt->declaration->kind != ast::TypeDeclKind::Record &&
         rt->declaration->kind != ast::TypeDeclKind::Choice) ||
        rt->genericArgs.empty())
        return;
    // fix #3143 (§14.1 / §2.5, v0.254): ONE branch. This used to be a local
    // (`rtd`) arm and #428's separate imported arm, byte-identical apart from
    // where they read the parameter NAMES from — the shape the audit
    // (docs/proposals/imported-symbol-audit-2026-08-13.md §4) names as the one
    // to collapse, because two copies of a predicate reading different sources
    // is how the `recordTypeOfRef` and §8.5-rendering divergences happened.
    // `sema::genericParamNames` reads the AST decl for a local record and
    // TypeSymbol::genericParams (ModuleLoader::convertGenericParams) for an
    // imported one, so both arms' behaviour is preserved exactly.
    const std::vector<std::string_view> gpNames =
        sema::genericParamNames(rt->declaration);
    if (!gpNames.empty() && gpNames.size() == rt->genericArgs.size()) {
        savedSubst = e->genericSubst_;
        savedSemaTy = e->genericSubstSemaTy_;
        std::unordered_map<std::string_view, llvm::Type*> newSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
        for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
            newSubst[gpNames[gi]] = e->lowerTypeRef(rt->genericArgs[gi]);
            // §14.1 (#766): resolve a bare-param arg through the outer subst.
            newSema[gpNames[gi]] = e->resolveTypeParamRef(rt->genericArgs[gi]);
        }
        e->genericSubst_ = std::move(newSubst);
        e->genericSubstSemaTy_ = std::move(newSema);
        active = true;
    }
}

Emitter::RecordSubstGuard::~RecordSubstGuard() {
    if (!active) return;
    e->genericSubst_ = std::move(savedSubst);
    e->genericSubstSemaTy_ = std::move(savedSemaTy);
}


// issue #462 / §14.1: resolve a record storage member's declared type through the
// active generic substitution. A bare type-parameter declared type (`T` — a
// single-part Named with no `declaration`) bound by `genericSubstSemaTy_` maps to
// its concrete sema type (`T` → `string`); anything else (a concrete declared
// type, or no active binding) returns the declared type unchanged. This is the
// same param-name lookup ownedTypeParamConcreteType / lowerNew's `new T()` path
// use, applied to the synthesized value-semantics members so a generic record's
// `T`-typed field compares / formats / hashes by its CONCRETE type's contract.
//
// #428 / §14.1 / §2.5: for IMPORTED generic record fields, recordMemberType
// returns null (no posParam, no FieldDecl AST), but the field type lives in the
// imported-member-type side table (ctx_.importedMemberType).  Fall back to that
// table so the substitution lookup below can still resolve T → the concrete type.
const sema::TypeRef* Emitter::recordMemberTypeResolved(const sema::Symbol* m) {
    const sema::TypeRef* declared = recordMemberType(m);
    // #428: imported field fallback — importedMemberType carries the TypeRef for
    // members whose declaration is null (all imported symbols).
    if (!declared) {
        if (auto* ms = dynamic_cast<const sema::MemberSymbol*>(m))
            if (ms->decl == nullptr)
                declared = ctx_.importedMemberType(ms);
    }
    if (!declared || genericSubstSemaTy_.empty()) return declared;
    if (declared->kind != sema::TypeRef::Kind::Named || declared->declaration)
        return declared;
    // §9.1 / §14.1 (#769): substitute the type param, PRESERVING the declared
    // field's nullable flag. The substitution map holds the bare type argument
    // (a non-nullable `int`), so a nullable field `V: T?` must resolve to
    // `int?` — not `int`. Without this the `?` was dropped, so a nullable
    // VALUE-type field lost its tagged `{i8 hasValue; T value}` treatment (wrong
    // struct layout / missing nullable wrap → the field read back wrong).
    auto withNullability = [&](const sema::TypeRef* sub) -> const sema::TypeRef* {
        if (declared->nullable && sub && !sub->nullable) {
            auto* nn = ctx_.makeTypeRef();
            *nn = *sub;
            nn->nullable = true;
            return nn;
        }
        return sub;
    };
    // Local generic record: type-param name is in astNode.
    if (declared->astNode && declared->astNode->name &&
        declared->astNode->name->parts.size() == 1) {
        auto it = genericSubstSemaTy_.find(declared->headName());
        if (it != genericSubstSemaTy_.end() && it->second)
            return withNullability(it->second);
    }
    // #428: imported generic record — type-param name is in importedName.
    if (!declared->importedName.empty()) {
        auto it = genericSubstSemaTy_.find(declared->importedName);
        if (it != genericSubstSemaTy_.end() && it->second)
            return withNullability(it->second);
    }
    return declared;
}


// The tagged struct for a nullable value type (prim, enum, or record). For an
// enum the value slot is the underlying integer (i32 — CG44); for a record
// (§9.4 / issue #416) it is the bare record struct (recordStruct); for a value
// primitive it is the primitive's LLVM type. Defined here (not inline in the
// header) because the record case needs recordStruct.
llvm::StructType* Emitter::nullableValueStructTy(const sema::TypeRef* rt) {
    // fix #3095 / §4.7 (v0.252): `()?` — "an optional with two values, `null`
    // and `()`". The tag carries both; the payload is ONE INERT BYTE
    // (unitPlaceholderTy) because §4.7 gives the unit type exactly one value,
    // so no byte pattern distinguishes one `()` from another and nothing ever
    // reads it. The two-element shape is deliberate (see isNullableUnitTy):
    // every family site that reads element 1 stays well-typed for `()?`.
    if (isNullableUnitTy(rt))
        return llvm::StructType::get(
            llvm_, {nullablePrimHasValTy(), unitPlaceholderTy()});
    if (isNullableEnumTy(rt))
        // §10.3 (#740): the nullable-enum payload slot is the enum's underlying
        // integer width (default i32), matching the non-nullable enum lowering.
        return llvm::StructType::get(
            llvm_, {nullablePrimHasValTy(), enumLlvmType(rt->declaration)});
    // per §7.2 / §8.6 / issue #416: a nullable record's payload is the bare
    // record struct, so the tag distinguishes null from any record value.
    // issue #462 / §14.1: a nullable GENERIC record (`Box<int>?` / `Box<string>?`)
    // must use the SPECIALIZED record struct for its payload — the same struct
    // the wrapper's construction and `!`/`?.` field access produce. Without the
    // substitution recordStruct returns the i32 placeholder layout, so the
    // wrapper's payload offsets disagree with the access side and `b!.value`
    // reads the wrong slot (garbage for `Box<int>?`; a corrupted 8-byte pointer
    // for `Box<string>?`). RecordSubstGuard installs `rt`'s concrete type args
    // for the recordStruct call, then restores the outer subst.
    if (isNullableRecordTy(rt)) {
        RecordSubstGuard guard(this, rt);
        return llvm::StructType::get(
            llvm_, {nullablePrimHasValTy(), recordStruct(rt->declaration)});
    }
    // fix #2447 / §35.9 / §7.2: a nullable choice's payload is the bare
    // choice struct ({i32 tag, case0_payload, ...} — choiceStruct), so the
    // leading hasValue tag distinguishes null from any choice value
    // (mirroring isNullableRecordTy just above; RecordSubstGuard is reused
    // here exactly as it is for a nullable GENERIC record — a generic
    // choice's payload struct is likewise per-instantiation, #2047).
    if (isNullableChoiceTy(rt)) {
        RecordSubstGuard guard(this, rt);
        return llvm::StructType::get(
            llvm_, {nullablePrimHasValTy(), choiceStruct(rt->declaration)});
    }
    // per §2.5–§2.6 / issue #1954: a nullable value-builtin's payload is the
    // NON-nullable builtin's lowering (DateTime/TimeSpan → i64, DateOnly/TimeOnly →
    // i32, Guid → {i64,i64}), so the leading hasValue tag distinguishes null from
    // every value. Lower a non-nullable copy (lowerTypeRef falls through to the
    // Named/value-builtin branch → guidStructType()/i64()/i32()) rather than the
    // Primitive slot below (rt->kind is Named, not Primitive — its `primitive`
    // field is unset, which is exactly what produced the malformed layout/GEP).
    if (isNullableValueBuiltinTy(rt)) {
        sema::TypeRef nn = *rt;
        nn.nullable = false;
        return llvm::StructType::get(
            llvm_, {nullablePrimHasValTy(), lowerTypeRef(&nn)});
    }
    // fix #2763 / §7.2 / §2.5–§2.6 / §4.14–§4.15 / §2.15 / §9.8 / §31.4: the
    // nullable VALUE-STRUCT BUILTIN family (Span/ReadOnlySpan/ArraySpan/
    // ReadOnlyArraySpan/ReadOnlyStringSpan → arraySpanStructType {ptr,i32,i32};
    // Range → rangeStructType {i64,i64,i64,i8}; JsonOptions → jsonOptionsStructType
    // {i1,i32,i1,i1,i32}; ExceptionInfo → {ptr,ptr,ptr}). Same shape as the
    // value-builtin arm above — the leading `hasValue` tag + the non-nullable
    // payload — but routed via isNullableValueStructBuiltinTy (which sees its
    // own name set, distinct from valueBuiltinName's, so the BOX-key set in
    // valueBuiltinBoxNameStr is unaffected). The bare-struct fall-through below
    // (`nullablePrimStructTy(rt->primitive)`) was the previous behavior and is
    // exactly the wrong-shape bug: a `Range<int>? := 1..5;` overwrote a 24-byte
    // Range into a 9-byte `{i8, i32}` placeholder slot, and the `= null` read
    // extracted a struct field rather than the non-existent tag, crashing
    // `bezelc` with SIGSEGV on the present-value initializer (measured on HEAD).
    if (isNullableValueStructBuiltinTy(rt)) {
        sema::TypeRef nn = *rt;
        nn.nullable = false;
        return llvm::StructType::get(
            llvm_, {nullablePrimHasValTy(), lowerTypeRef(&nn)});
    }
    return nullablePrimStructTy(rt->primitive);
}


// fix #2903 / §5.1 / §7.2 / §23.1 / §9.2: see the declaration comment in
// LlvmBackend_impl.hpp for why this exists and what it is the one definition of.
// fix #2903 / §7.2 (factored out by #2925): the width a value must arrive at to
// reach `slotTy`. A nullable VALUE slot is the tagged `{i8 hasValue; T}` struct
// and the value is the bare payload `T`, so the width to match is T's. The
// shape test is the EXACT one #2888 established — a nullable value type is a
// LITERAL two-element struct whose element 0 is the i8 tag, while every class,
// record and choice is a NAMED struct — but with the payload left unrestricted,
// unlike `nullableValuePayloadTy`, which narrows to the scalars `emitCompoundOp`
// can operate on: a nullable RECORD / Guid payload is an aggregate this must
// still recognize (so it does not target the whole tagged struct) and then pass
// through untouched.
llvm::Type* Emitter::slotPayloadTy(llvm::Type* slotTy) {
    if (auto* st = llvm::dyn_cast<llvm::StructType>(slotTy))
        if (st->isLiteral() && st->getNumElements() == 2 &&
            st->getElementType(0) == nullablePrimHasValTy())
            return st->getElementType(1);
    return slotTy;
}

llvm::Value* Emitter::coerceInitializerToSlot(llvm::Value* v,
                                              llvm::Type* slotTy,
                                              bool srcUnsigned) {
    if (!v || !slotTy) return v;
    llvm::Type* want = slotPayloadTy(slotTy);
    if (v->getType() == want) return v;
    // §5.1's WIDENING direction (int→long, int→double, float→double, int→decimal).
    llvm::Value* w = coerceNumericToType(v, want, srcUnsigned);
    if (w->getType() == want) return w;
    // §5.1's "Constant narrowing" direction: an integer strictly WIDER than the
    // integer slot. `i1` (`bool`) is excluded on both sides — §5.1's "Between
    // integers and `bool`" row makes that conversion never implicit, so a
    // mismatch there is a shape this function must not touch.
    if (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(1) &&
        want->isIntegerTy() && !want->isIntegerTy(1) &&
        v->getType()->getIntegerBitWidth() > want->getIntegerBitWidth())
        return builder_.CreateTrunc(v, want, "init.narrow");
    return v;  // no §5.1 conversion applies — leave the value as is.
}

// fix #2925 (§5.1 "Constant narrowing" at a CALL ARGUMENT / `return`, v0.240) —
// see the declaration comment in LlvmBackend_impl.hpp for why this is NOT
// `coerceInitializerToSlot` and what the placeholder truncation was.
llvm::Value* Emitter::coerceExprValueToSlot(llvm::Value* v, llvm::Type* slotTy,
                                            const ast::Expr* e,
                                            bool srcUnsigned) {
    if (!v || !slotTy) return v;
    // fix #4056 / §9.4 / CG29 / §22.3 (v0.294): inside a record method or
    // constructor `this` is kept as a POINTER to the by-value receiver slot
    // (CG29, §9.4 — spilled so `this.field` reuses record field access). When
    // `this` reaches a CALL ARGUMENT position whose callee expects the record
    // STRUCT — a static method's record parameter (`R.Touch(this)`), or a
    // non-`this` instance-method parameter of a record type (`this.Combine(this)`
    // → the non-receiver arg) — the load needs to happen HERE, exactly the way
    // `emitReturn` loads it for `return this` (fix #1759). Without this the
    // ptr reaches the call site unchanged and LLVM's verifier rejects the
    // module (`Call parameter type does not match function signature! / IR
    // verification failed`). A no-op for every other value (v !=
    // currentThis_), every non-record receiver (class/enum), and every
    // non-matching destination type — `coerceThisRecordValue`'s own equality
    // guard rejects them and returns v unchanged.
    v = coerceThisRecordValue(v, slotTy);
    llvm::Type* want = slotPayloadTy(slotTy);
    if (v->getType() == want) return v;
    // §5.1's WIDENING direction is unconditional and unchanged — the target
    // type is only ever a widening's *destination*, never its authority over
    // what the value IS (issues #408 / #421 / #410 / #746 / #1778).
    llvm::Value* w = coerceNumericToType(v, want, srcUnsigned);
    if (w->getType() == want) return w;
    // §5.1's "Constant narrowing" direction. THE GATE: only when sema already
    // types the producing expression AT this width, so codegen is repairing a
    // width its own lowering failed to honour (a re-typed `const K := 3` still
    // loads its declared i32 slot; `1 + 2` still folds at i32) rather than
    // performing a conversion of its own. Without the gate, an IMPORTED GENERIC
    // METHOD's i32 PLACEHOLDER parameter (#1943 / #428) looked like a narrower
    // slot and `Count<long>(y)` had its i64 silently truncated to fit it —
    // MEASURED compiling, linking and running (returning 7) instead of raising
    // #1943's fail-closed diagnostic.
    if (!e) return w;
    const sema::TypeRef* st = ctx_.exprType(*e);
    if (!st || st->kind != sema::TypeRef::Kind::Primitive) return w;
    if (lowerPrimitive(st->primitive) != want) return w;
    return coerceInitializerToSlot(v, want, srcUnsigned);
}


// fix #568 / §14.1 / §7.1: see the declaration comment in LlvmBackend_impl.hpp.
llvm::Value* Emitter::coerceReturnToNullableValue(llvm::Value* v,
                                                  const sema::TypeRef* declRet,
                                                  bool srcUnsigned,
                                                  bool sourceIsNullableRef,
                                                  const ast::Expr* srcExpr) {
    if (!v || !declRet) return v;
    // Resolve the EFFECTIVE nullable value type. A bare nullable type-parameter
    // (`T?`) resolves through the active monomorphization to its concrete value
    // type; a non-generic nullable value type (`int?` / `Pt?`) is used directly.
    sema::TypeRef nvBuf;
    const sema::TypeRef* nv = nullptr;
    if (isNullableValueTy(declRet)) {
        nv = declRet;
    } else if (declRet->nullable &&
               declRet->kind == sema::TypeRef::Kind::Named &&
               !declRet->declaration && !genericSubstSemaTy_.empty()) {
        if (nullableTypeParamConcrete(declRet, nvBuf) && isNullableValueTy(&nvBuf))
            nv = &nvBuf;
    }
    if (!nv) return v;  // reference `?` / non-nullable / unresolved — untouched.

    llvm::StructType* nst = nullableValueStructTy(nv);
    if (v->getType() == nst) return v;  // already the tagged struct.
    // `return null` lowered to a null pointer → the {i8 0, T 0} null constant.
    if (v->getType()->isPointerTy() && llvm::isa<llvm::ConstantPointerNull>(v))
        return nullableValueNull(nv);
    // #1767 / §19.2: the returned expression's SEMA type is a nullable REFERENCE
    // (`Object?`) but the declared return is a nullable VALUE (`int?`), and the
    // value is a RUNTIME `ptr` — not a constant. This is the all-null `case … end`
    // shape: a `null` literal defaults to `Object?` (§7 nullability), so a case
    // whose every arm is `null` has sema result `Object?` and lowers to a bare
    // `ptr` PHI (each arm's `null` → ConstantPointerNull, joined). Sema admits a
    // reference-nullable value into a value-nullable return ONLY when it is
    // provably null (no non-null reference type is assignable to `int?`, §20), so
    // the runtime ptr is null → the {i8 0, T 0} constant. This is gated on
    // `sourceIsNullableRef` so a genuine repr bug where a value-nullable source
    // (`int?`) mis-lowers to a `ptr` (e.g. an unrelated `?.` / LINQ `*_or_null`
    // divergence) is NOT masked — those still fall through to the verifier. Before
    // this, only the ConstantPointerNull case above was handled; the runtime PHI
    // fell through to emitReturn (also constant-null-only) → `ret ptr` from a
    // `{i8,i32}` fn — malformed IR the JIT's missing verifier silently ran.
    if (sourceIsNullableRef && v->getType()->isPointerTy())
        return nullableValueNull(nv);

    llvm::Type* slotTy = nst->getElementType(1);
    // #843 / §5.1 / §7: widen/convert a NARROWER numeric return value to the
    // nullable's inner type before wrapping — `return 42` from a `long?` method
    // needs the i32 literal i32→i64, `return someInt` from a `double?` method
    // needs SIToFP. Without this the bare narrower value fell through unwrapped
    // and emitReturn then `ret`'d it against the `{i8, T}` struct return type
    // (IR-verify failure). Mirrors the local-decl fix (#842).
    // fix #2925 (§5.1 "Constant narrowing" composed with "Nullable
    // conversions", v0.240): `coerceExprValueToSlot` rather than
    // `coerceNumericToType`, which is the WIDENING direction only. §5.1: "The
    // two rules compose through at most one `?` on each side ([§7.10]: `?` is
    // idempotent): `S` converts to `T?` via `S → T → T?`" — so `return K;`
    // from a `byte?`-returning method (a `const` identifier that loads its i32
    // slot) must reach the tagged payload as an i8 before the wrap. `slotTy`
    // is already the unwrapped payload, so no further tagged retarget occurs.
    // The narrowing carries the same sema-types-it-at-the-slot GATE as every
    // other value-reaching-a-slot site (see coerceExprValueToSlot): a caller
    // that passes no `srcExpr` keeps the widening-only, pre-#2925 behaviour.
    if ((v->getType()->isIntegerTy() || v->getType()->isFloatingPointTy()) &&
        v->getType() != slotTy) {
        llvm::Value* inner =
            coerceExprValueToSlot(v, slotTy, srcExpr, srcUnsigned);
        if (inner->getType() == slotTy) return wrapNullableValue(nv, inner);
    }
    // A bare value matching the value slot — or a layout-equal but differently-
    // named aggregate (the specialized-vs-unspecialized record struct mismatch a
    // monomorphized body induces, #460) — is wrapped. wrapNullableValue
    // reinterprets the latter to the slot type before insertvalue.
    if (v->getType() == slotTy) return wrapNullableValue(nv, v);
    if (auto* vSt = llvm::dyn_cast<llvm::StructType>(v->getType()))
        if (auto* sSt = llvm::dyn_cast<llvm::StructType>(slotTy))
            if (vSt->isLayoutIdentical(sSt)) return wrapNullableValue(nv, v);
    return v;  // shapes incompatible — leave to emitReturn's own coercion.
}


llvm::Value* Emitter::lowerTypeTest(const ast::TypeTestExpr& te) {
    if (!te.operand || !te.testType) return nullptr;
    // fix #2865 (§8.6 "Boxing" / §28.9 / §28.3, v0.240): an UNBOUND +1 box
    // temporary. §8.6 mints a fresh heap wrapper whenever a value type reaches
    // an `Object`/interface location, and §28.9 makes it runtime-managed —
    // "exempt from MANUAL disposal", which removes the obligation from the USER,
    // not from codegen. When the box is BOUND to a local, `collectBoxRelease
    // Eligible` already arms a scope-end `bz_box_release`; when it is a bare
    // temporary — `if m.M() is int then`, `if e[i] is int then` — nothing ever
    // held it and nothing ever freed it: one 24-byte box per evaluation, growing
    // without bound in a loop (#2865 shapes 1 and 2, 480 B / 20 iterations).
    //
    // `is` / `is not` / `as <value type>` are precisely the positions
    // BoxEscapeWalker already classifies as NON-CONSUMING reads: `is` walks the
    // descriptor chain and keeps nothing, and a value-target `as` COPIES the
    // payload bytes out of the wrapper. So once the operand is proven to be a
    // fresh, solely-owned box, releasing it here is exactly-once.
    //
    // TWO independent proofs, either of which suffices:
    //   * the mint-identity check below (`lastBoxMint_`), which covers every
    //     box minted IN THIS FRAME while lowering the operand — the inline
    //     indexer/property read paths (shape 2);
    //   * `isUserMethodBoxReturnCall`, the SAME predicate `isPlusOneBoxProducer`
    //     case (b) uses to arm the bound-local release, which covers a box
    //     minted by a locally-compiled `boxReturnMethods_` callee at its return
    //     edge and transferred to us (shape 1).
    // Anything else keeps leaking — the safe direction (a bounded leak over a
    // double free), and what the filed residuals track.
    llvm::Value* savedBoxMint = lastBoxMint_;
    lastBoxMint_ = nullptr;
    // fix #3090 (§12.7 / §8.6, v0.252): a type test whose operand is a BARE
    // IDENTIFIER lowers that identifier as its §8.6 BOX, never as its
    // §12.7-narrowed value — §12.7: "the operation inspects the runtime type tag
    // without moving from the operand", and that tag is the wrapper's vtable
    // word. Without the suppression a NESTED test (`if o is int then … o is T
    // … end`) handed `emitIsA` an `i32` and walked a descriptor chain off it. The
    // answer is unchanged either way (a narrowed `o` and its box carry the same
    // runtime type), so that position keeps its pre-fix lowering exactly.
    //
    // GATED on the identifier shape, and the gate is LOAD-BEARING rather than
    // tidiness: the flag suppresses for the whole lowering it brackets, so an
    // ungated increment also silenced the unbox for a narrowed identifier nested
    // INSIDE a compound operand — `if o is int then if Take(o) is Foo then … end;
    // end;` handed a `ptr` to an `i32` parameter and failed IR verification.
    // Sema accepts that program only BECAUSE this fix widened the narrowing, so
    // the crash shape would have been a new one. A bare identifier is also the
    // only shape `narrowedIdentDeclaredType` — the classification half this
    // suppression must stay in step with — can answer for at all, so both halves
    // now key on exactly the same condition.
    //
    // The gate does NOT weaken the case the classification half exists for: in
    // `if ostr is string and not (ostr is decimal) then …` the inner test's
    // operand IS the bare identifier `ostr`, so the suppression still applies
    // there and #2793's sweep still reads 4095. The two requirements are
    // disjoint, not in tension.
    const bool suppressOperandUnbox =
        dynamic_cast<const ast::IdentifierExpr*>(te.operand.get()) != nullptr;
    if (suppressOperandUnbox) ++narrowedUnboxSuppressed_;
    llvm::Value* obj = lowerExpr(*te.operand);
    if (suppressOperandUnbox) --narrowedUnboxSuppressed_;
    const bool freshBoxTemp =
        obj && obj->getType()->isPointerTy() &&
        ((lastBoxMint_ && obj == lastBoxMint_) ||
         isUserMethodBoxReturnCall(te.operand.get()));
    lastBoxMint_ = savedBoxMint;
    // Release the operand's wrapper. Call ONLY where `obj` is provably dead:
    // never on an `as` whose target is a REFERENCE type, because that arm
    // RETURNS `obj` itself (`o as Object`, and `o as IShape` on a boxed record
    // whose descriptor matches) — releasing there would hand the caller a
    // dangling box.
    auto releaseFreshBoxTemp = [&] {
        if (freshBoxTemp) emitBoxRelease(obj);
    };
    if (!obj) return nullptr;
    sema::TypeRef* rt = ctx_.resolvedType(*te.testType);
    if (!rt) return nullptr;
    // fix #3092 narrow (§12.9/§8.6, v0.254): a BARE type-parameter target must
    // be substituted through the active monomorphization so the descriptor
    // resolution paths below can find its runtime tag. The shape is narrow on
    // purpose — ONLY a bare-parameter placeholder (Named, declaration==nullptr,
    // genericArgs.empty — a null-decl-with-args shape is a CONSTRUCTED null-decl
    // builtin like `Array<T>`, recursed into by perInstDescriptorForTarget / box-
    // DescriptorFor themselves) is rewritten. For a constructed generic target
    // that MENTIONS a type parameter (e.g. `Wrap<T>`) the outer substitution
    // would mask the bare T from rejectCollapsedTypeTestTarget above (which
    // detects a collapsed-target instantiation via per-argument T mention) —
    // that was the broad-vs-narrow lesson Wave 2's revert taught (commits
    // 7a35a0c / b99f20d / aaad17f): the broad substitute over EVERY target let
    // `Wrap<(int)->int>` (substituted from `Wrap<T>`) through the collapse-guard
    // because the substituted arg was no longer "different from the original",
    // and the test fell back to the shared class tag → the #2192 / #2012 /
    // #2202 false-positive matches that gated the broad reject. Restricting to
    // the bare-parameter shape preserves the #3092 fix without disturbing the
    // #2069 per-monomorphization collapse-reject. The predicate is a no-op on
    // every other target (Named-with-declaration, Primitive, InlineArray, Unit,
    // Function, Range, …), so the existing fast paths are unaffected.
    if (rt->kind == sema::TypeRef::Kind::Named &&
        !rt->declaration &&
        rt->genericArgs.empty() &&
        !genericSubstSemaTy_.empty()) {
        if (const sema::TypeRef* sub =
                substituteTypeRefLocal(rt, genericSubstSemaTy_);
            sub != rt) {
            rt = const_cast<sema::TypeRef*>(sub);
        }
    }

    // fix #2775 (§8.6/§12.7/§19.3, v0.240): SILENCE AMPLIFIER. Every
    // `return nullptr` below is not "an error we recover from" — lowerIf turns
    // a null condition into an unconditional branch PAST the body (see the note
    // there), so a dropped type test reads as a constant `false` in a program
    // that compiled clean. Two shipped defects (#2703's collection-literal arm
    // and #2761) each had "no diagnostic at all" as their worst failure mode.
    //
    // ONE definition, called from every bail, rather than a guard copied per
    // site: the copies are how a predicate acquires the missing case (the
    // #2787 lesson). Follows the #2491 precedent — Warning, suppressed once the
    // compile already has errors so a cascade never buries the real diagnostic.
    //
    // The `dropped` classification deliberately EXCLUDES the shapes where a
    // non-answer is legitimate: a declaration-less Named target is the bare
    // type-PARAMETER shape, whose descriptor does not resolve inside the
    // UNSPECIALIZED generic template body (issue #490 — that stub is never
    // executed; the specialization path compiles the concrete version), and an
    // operand whose sema type is not Object/an interface is erased there for
    // the same reason. Warning on either would fire on dead code.
    auto reportDroppedTest = [&](const char* why) {
        if (ctx_.diagnostics().hasErrors()) return;
        // A TypeTestExpr synthesized inside a chain can carry an empty span
        // (line 0), which rendered as a useless `file:0:0` — the #2737 shape.
        // Fall back to the operand, then to the tested type, so the report
        // always names a real position. (`line` is 1-based, so 0 means unset.)
        SourceLocation at = te.span.begin;
        if (at.line == 0 && te.operand) at = te.operand->span.begin;
        if (at.line == 0 && te.testType) at = te.testType->span.begin;
        ctx_.diagnostics().report(
            Severity::Warning, at,
            std::string("internal: this type test was DROPPED and reads as "
                        "false — ") + why +
                " (a value type reaching an Object/interface location must be "
                "boxed, §8.6) — #2775");
    };
    // fix #3090 (§12.7 / §8.6, v0.252): the operand's type AS LOWERED HERE. The
    // suppression above lowered a §12.7-narrowed identifier as its §8.6 box /
    // declared reference, so every classification below must read the DECLARED
    // type, not the narrowed one — otherwise the two disagree and the decisions
    // are made about a value that was never produced. This was a live SIGSEGV:
    // after `if ostr is string and not (ostr is decimal) then …`, the second
    // conjunct's `ostr` types as `string`, so `mightHoldString` answered FALSE
    // for an operand that is exactly a bare vtable-less `bz_string*` and emitIsA
    // walked its `char* data` as a descriptor chain (MEASURED: #2793's
    // `other_primitive_boxes_are_unchanged` sweep, 4095 -> core dump).
    // `narrowedIdentDeclaredType` declines for every non-narrowed operand, so
    // this is `ctx_.exprType` everywhere else.
    auto operandStaticTy = [&]() -> const sema::TypeRef* {
        if (const sema::TypeRef* d = narrowedIdentDeclaredType(*te.operand))
            return d;
        return ctx_.exprType(*te.operand);
    };
    // True when the OPERAND is statically a reference location (Object or an
    // interface): sema has already promised a pointer, so a non-pointer lowered
    // value is by construction a producer that failed to establish the §8.6
    // boxed representation.
    auto operandIsReferenceLocation = [&]() {
        const sema::TypeRef* opTy = operandStaticTy();
        return opTy && isObjectOrInterfaceTarget(opTy);
    };
    // §8.6 / §12.7 (#417): `is string` / `as string`. A string stored in an
    // Object is a bare `bz_string*` with NO leading vtable/descriptor word, so
    // emitIsA's descriptor walk cannot run on it. Recognize it via
    // bz_obj_is_string, which tells a string (word 0 is `char* data`, not a
    // listed vtable) from a class/box (word 0 is a vtable) using the
    // module-global vtable table. Only an OBJECT operand reaches here as a
    // pointer; a string-typed operand `s is string` is constant-true and never
    // produces a TypeTest the front-end keeps. `as string` selects the operand
    // (already the bz_string*) or null.
    if (rt->kind == sema::TypeRef::Kind::Primitive &&
        rt->primitive == sema::PrimitiveType::String) {
        if (!obj->getType()->isPointerTy()) {
            if (operandIsReferenceLocation())
                reportDroppedTest("the operand did not lower to a reference");
            return nullptr;
        }
        auto* isStrFn = declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
        llvm::Value* isStr = builder_.CreateCall(
            isStrFn, {obj, vtableTablePtr()}, "is.string");
        llvm::Value* isStrBit =
            builder_.CreateICmpNE(isStr, builder_.getInt32(0));
        if (te.op == "is") { releaseFreshBoxTemp(); return isStrBit; }
        if (te.op == "is not") {
            releaseFreshBoxTemp();
            return builder_.CreateXor(isStrBit, builder_.getInt1(true));
        }
        if (te.op == "as")
            // §17.4: `o as string` → the bz_string* on success, null otherwise.
            return builder_.CreateSelect(
                isStrBit, obj, llvm::ConstantPointerNull::get(ptrTy()));
        return nullptr;
    }
    // §8.6 / §12.7 (#417): `is Object` / `as Object`. Every non-null reference
    // is an Object (the universal root, §8.1). `o is Object` is true for any
    // non-null operand; `o as Object` is the operand itself (no downcast).
    if (rt->kind == sema::TypeRef::Kind::Primitive &&
        rt->primitive == sema::PrimitiveType::Object) {
        if (!obj->getType()->isPointerTy()) {
            if (operandIsReferenceLocation())
                reportDroppedTest("the operand did not lower to a reference");
            return nullptr;
        }
        llvm::Value* nonNull = builder_.CreateICmpNE(
            obj, llvm::ConstantPointerNull::get(ptrTy()), "is.object");
        if (te.op == "is") { releaseFreshBoxTemp(); return nonNull; }
        if (te.op == "is not") {
            releaseFreshBoxTemp();
            return builder_.CreateXor(nonNull, builder_.getInt1(true));
        }
        // `as Object` RETURNS the operand — no release (see releaseFreshBoxTemp).
        if (te.op == "as")
            return builder_.CreateSelect(
                nonNull, obj, llvm::ConstantPointerNull::get(ptrTy()));
        return nullptr;
    }
    // §12.7/§2.8.3 rule 4/§19.3 (#2012, per the #2069 ruling): at THIS monomorphization
    // the target's type-parameter argument may substitute to a runtime-erased
    // kind (a function type) whose descriptor key collapses onto a sibling
    // instantiation, so `o is Wrap<T>` (T=(int)->int) would match a
    // Wrap<(string)->string> value and `as` reinterpret it — REJECT (an
    // implementation-limitation diagnostic; lifts under #1939 reification). Return
    // a well-formed placeholder of the op's result type; the reported Error fails
    // the compile (Emitter::run → nullptr, #628 part 1).
    if (rejectCollapsedTypeTestTarget(rt, te.span.begin)) {
        if (te.op == "as") return llvm::ConstantPointerNull::get(ptrTy());
        return builder_.getInt1(false);
    }
    // §8.6 / §12.7: resolve the target's runtime descriptor. For a reference
    // type (class/interface) this is its descArray_; for a boxable VALUE type
    // (record/enum) or a value primitive it is the box descriptor built in
    // setupBoxDescriptors — this is what makes `o is Rect` / `o is int`
    // recognize a boxed value (findings 06/08/12/13).
    llvm::GlobalVariable* targetDesc = boxDescriptorFor(rt);
    if (!targetDesc) {
        // Reference types declared inline still resolve via descArray_.
        if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration) {
            auto dit = descArray_.find(rt->declaration);
            if (dit != descArray_.end()) targetDesc = dit->second;
        }
    }
    // #1843 (§12.7/§12.9): for a CONSTRUCTED generic class/interface target
    // (`is Box<int>`), resolve the per-monomorphization descriptor so the test
    // honors the type argument — the shared `descArray_[decl]` above is the
    // erased class tag and would match every instantiation (type confusion).
    if (llvm::GlobalVariable* pid = perInstDescriptorForTarget(rt))
        targetDesc = pid;
    // #596 / §21 / §12: built-in exception types (e.g. Exception,
    // ArgumentException) have no user TypeSymbol (declaration == nullptr),
    // so neither boxDescriptorFor nor the descArray_ lookup above resolves
    // them.  Fall back to builtinDesc_ via the type name so that `e is
    // Exception` and `e as Exception` work when `e`'s runtime type is a user
    // class that inherits Exception.
    if (!targetDesc && te.testType && te.testType->name &&
            !te.testType->name->parts.empty()) {
        std::string tn(te.testType->name->parts.back());
        auto bit = builtinDesc_.find(tn);
        if (bit != builtinDesc_.end()) targetDesc = bit->second;
    }
    if (!targetDesc) {
        // #2775: a CONCRETE target with no runtime descriptor means the
        // compiler has no runtime tag for it — an internal limitation, not a
        // program error. Measured live when this guard landed: `var o: Object
        // := 12.5m; o is decimal` and `var o: Object := ia; o is [3]int` both
        // compiled clean and answered false, because neither `decimal` nor an
        // inline array is boxed although §8.6 requires both.
        const bool concreteTarget =
            rt->kind == sema::TypeRef::Kind::Primitive ||
            rt->kind == sema::TypeRef::Kind::Unit ||
            rt->kind == sema::TypeRef::Kind::InlineArray ||
            (rt->kind == sema::TypeRef::Kind::Named && rt->declaration);
        if (concreteTarget)
            reportDroppedTest("no runtime descriptor resolved for the target "
                              "type");
        return nullptr;
    }
    // §12.7 / §7 / issues #447 + #448: a nullable VALUE type operand is a
    // tagged {i8 hasValue; T value} struct, NOT a pointer. emitIsA walks a
    // vtable descriptor chain and requires a pointer operand; calling it on a
    // struct produces illegal IR (SIGILL). For `x is T` where x: T? the
    // semantics per §12.7 reduce to "is the value present?" — i.e. hasValue.
    // Handle the three nullable-value subtypes (prim/enum/record) together:
    // the operand type is the scrutinee's TypeRef, obtained from sema.
    if (!obj->getType()->isPointerTy()) {
        // Only the nullable-VALUE-type struct case is handled here (nullable
        // CLASS types lower to ptr and never reach this branch).
        const sema::TypeRef* operandTy = operandStaticTy();  // fix #3090
        if (operandTy && isNullableValueTy(operandTy) &&
            obj->getType()->isStructTy()) {
            llvm::Value* hasVal = nullablePrimHasValue(obj);  // i1
            if (te.op == "is") return hasVal;
            if (te.op == "is not") return builder_.CreateNot(hasVal);
        }
        // fix #3120 (§12.7 / §8.6 / §17.4, v0.252): a CONCRETE non-nullable
        // VALUE-type operand (a primitive / record / enum / value-builtin /
        // inline array / unit whose sema type is not a type parameter) lowers
        // here to a non-pointer value. There is no runtime tag to consult — a
        // non-boxed value type has exactly one static type — so the test is
        // decided STATICALLY at the operand's declared type: target type
        // identical to the operand type → `true`; otherwise → `false`. §12.7:
        // "`is T` tests whether a value is an instance of `T`", and a `Pt` is
        // an instance of `Pt`. The `concreteValueTypeOperand` predicate
        // excludes the generic-template shape (operand type is a bare type
        // parameter — Named with declaration == nullptr, the #490 / #2775
        // unspecialized body that is never executed) so a type-parameter
        // operand keeps its pre-#3120 silent-drop and `reportDroppedTest`
        // still gates on the reference-location shape.
        if (operandTy && concreteValueTypeOperand(operandTy)) {
            const bool sameTy = staticTypeIdentical(operandTy, rt);
            if (te.op == "is") return builder_.getInt1(sameTy);
            if (te.op == "is not") return builder_.getInt1(!sameTy);
            if (te.op == "as") {
                // §17.4: `x as T` produces `T?` — a wrapped copy on a
                // compatible unbox, `null` otherwise. The operand is already
                // the value (no box to unbox); a successful match wraps it as
                // {i8 1, value}; a mismatch is the all-zero `null`. The
                // boxed-by-target direction (line ~2779) handles the more
                // general Object/interface operand; this arm covers the
                // concrete-value-typed operand where the match is decided at
                // the operand's static type.
                if (sameTy) {
                    auto* st = nullableValueStructTy(rt);
                    llvm::Value* wrapped = llvm::UndefValue::get(st);
                    wrapped = builder_.CreateInsertValue(
                        wrapped, builder_.getInt8(1), 0, "as.hasv");
                    wrapped = builder_.CreateInsertValue(
                        wrapped, obj, 1, "as.wrap");
                    return wrapped;
                }
                return nullableValueNull(rt);
            }
            return nullptr;
        }
        // #2775: sema already typed this operand as `Object` or an interface —
        // both REFERENCE locations — so a non-pointer lowered value here is by
        // construction a codegen invariant violation: some producer failed to
        // establish the §8.6 boxed representation.
        if (operandIsReferenceLocation())
            reportDroppedTest("the operand did not lower to a reference");
        return nullptr;
    }
    // #1071 / §8.6 / §12.7: an Object/interface operand may hold a bare
    // bz_string* — a string has NO leading vtable/descriptor word (see the
    // `is string` target branch above). emitIsA loads word-0 as a descriptor
    // pointer and walks it; on a string that word is the `char* data` pointer,
    // so the walk dereferences garbage → SIGSEGV. A string is never any other
    // concrete type, so when the operand could be a string, gate emitIsA on
    // bz_obj_is_string: a string yields isA=false (`is`→false, `is not`→true,
    // `as`→null) and only a genuine box/class runs the descriptor walk.
    bool mightHoldString = false;
    if (const sema::TypeRef* opTy = operandStaticTy()) {  // fix #3090
        if (opTy->kind == sema::TypeRef::Kind::Primitive &&
            opTy->primitive == sema::PrimitiveType::Object) {
            mightHoldString = true;
        } else if (opTy->kind == sema::TypeRef::Kind::Named && opTy->declaration &&
                   opTy->declaration->kind == ast::TypeDeclKind::Interface) {
            mightHoldString = true;
        }
    }
    llvm::Value* isA;
    if (mightHoldString) {
        auto* isStrFn = declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
        llvm::Value* isStr = builder_.CreateICmpNE(
            builder_.CreateCall(isStrFn, {obj, vtableTablePtr()}, "isa.isstr"),
            builder_.getInt32(0));
        llvm::BasicBlock* preGuardBB = builder_.GetInsertBlock();
        auto* notStrBB = llvm::BasicBlock::Create(llvm_, "isa.notstr", currentFn_);
        auto* afterGuardBB =
            llvm::BasicBlock::Create(llvm_, "isa.guarded", currentFn_);
        builder_.CreateCondBr(isStr, afterGuardBB, notStrBB);
        builder_.SetInsertPoint(notStrBB);
        llvm::Value* isAReal = emitIsA(obj, targetDesc);
        llvm::BasicBlock* emitEndBB = builder_.GetInsertBlock();
        builder_.CreateBr(afterGuardBB);
        builder_.SetInsertPoint(afterGuardBB);
        auto* phi = builder_.CreatePHI(i1(), 2);
        phi->addIncoming(builder_.getInt1(false), preGuardBB);  // string → no match
        phi->addIncoming(isAReal, emitEndBB);
        isA = phi;
    } else {
        isA = emitIsA(obj, targetDesc);
    }
    if (te.op == "is") { releaseFreshBoxTemp(); return isA; }
    if (te.op == "is not") {
        releaseFreshBoxTemp();
        return builder_.CreateXor(isA, builder_.getInt1(true));
    }
    if (te.op == "as") {
        // §17.4: `x as T` for a VALUE-type target unboxes — load the value
        // bytes out of the box `{ vtable*, rc, value-bytes }` (payload at two
        // words in — past the vtable word and the #422 rc word, BZ_BOX_RC_OFFSET).
        // For a value primitive OR a record (issue #416) the result is `T?` (a
        // tagged {hasValue, value} struct — non-null on a compatible unbox, null
        // otherwise per §8.6 "Unboxing reverses the process"). A reference-type
        // target keeps the #399 behavior (the pointer IS the value). The payload
        // load is guarded by `isA` so a failed cast (or a null operand) never
        // dereferences the box.
        // fix #2915: `rt->kind == Unit` is excluded EXPLICITLY rather than by
        // isBoxableValueType (which now admits it, so the boxed `()` has a
        // runtime identity — §4.7/§8.6). This arm loads the payload as
        // `lowerTypeRef(rt)`, and the unit type lowers to LLVM `void`: a
        // `load void` is not constructible and would abort the compiler. The
        // unboxing direction `o as ()` therefore keeps EXACTLY its pre-fix
        // lowering (it falls through to the reference-target arm below) — this
        // change makes the boxing and type-TEST directions correct and leaves
        // the unbox untouched, rather than inventing a `()?` representation.
        // Filed separately; it is the unit twin of #2911's `x as [N]T`.
        if (isBoxableValueType(rt) && rt->kind != sema::TypeRef::Kind::Unit) {
            // §17.4: the result of `as` is ALWAYS nullable, so the target may be
            // written either bare (`o as int`) or already-nullable (`o as int?`).
            // The box stores the BARE value bytes, and the result struct built
            // below is itself the tagged {hasValue, T}. So `valTy` — the type of
            // the unboxed payload and of the value slot we fill — must be the
            // NON-NULLABLE underlying type. Without stripping, a nullable target
            // (`int?`/`Rec?`) made `valTy` the {i8, T} struct, so the load read a
            // {i8,T} and the wrap below inserted that {i8,T} into the T slot of
            // another {i8,T} → malformed insertvalue (issue #1154).
            sema::TypeRef rtBase = *rt;
            rtBase.nullable = false;
            llvm::Type* valTy = lowerTypeRef(&rtBase);
            auto* loadBB = llvm::BasicBlock::Create(llvm_, "unbox.load",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "unbox.cont",
                                                    currentFn_);
            llvm::BasicBlock* preBB = builder_.GetInsertBlock();
            builder_.CreateCondBr(isA, loadBB, contBB);

            builder_.SetInsertPoint(loadBB);
            // value-bytes live two words in: past the vtable word and the #422
            // rc word (boxValue lays out { vtable*, i64 rc, payload }).
            llvm::Value* payload = builder_.CreateGEP(
                ptrTy(), obj, {builder_.getInt32(2)}, "unbox.ptr");
            llvm::Value* loaded = builder_.CreateLoad(valTy, payload, "unbox");
            builder_.CreateBr(contBB);

            builder_.SetInsertPoint(contBB);
            // fix #2865: the payload has been LOADED out of the wrapper (a
            // by-value copy of the scalar/struct bytes — `loaded` above), so on
            // this arm, and only on this arm, `obj` is dead and a fresh temporary
            // wrapper can be reclaimed. Both PHI predecessors join here, so the
            // release runs exactly once on the matched and the unmatched path
            // alike (bz_box_release is null-safe for the null-operand case).
            // The reference-target `as` below deliberately does NOT release.
            //
            // ⚠ The call is emitted AFTER this block's PHI nodes, never before:
            // an emitBoxRelease at the top of contBB put a `call` ahead of them
            // and the verifier rejected the module ("PHI nodes not grouped at
            // top of basic block!") — caught by this fix's own `as int` probe.
            if (rt->kind == sema::TypeRef::Kind::Primitive ||
                isNullableRecordTy(rt) ||
                valueBuiltinName(rt) != nullptr ||
                (rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
                 (rt->declaration->kind == ast::TypeDeclKind::Record ||
                  rt->declaration->kind == ast::TypeDeclKind::Enum))) {
                // §8.6 / §17.4 (issue #416, enum #1072, value-builtin #1953): a
                // value primitive, record, ENUM, or VALUE-BUILTIN (DateTime/…/Guid)
                // unboxes to a non-null T? on a compatible cast, a null T?
                // otherwise. The result is the tagged {hasValue, payload} struct:
                // hasValue=1 + the loaded payload on success, the zero struct on
                // failure. For a record/Guid the payload is the bare struct; for a
                // primitive/enum/DateTime it is the scalar (the enum's underlying
                // integer, §10.3). `rt` is the (non-nullable) target type, so build
                // the nullable form's tagged struct — without this an `enum?` from
                // `as` was a bare value, and the downstream `int(enum?)` / `<> null`
                // / `!` GEP'd its tag on a non-struct (#1072 LLVM checkGEPType
                // assertion). For a value-builtin, a FAILED cast now yields null
                // rather than the zero value / MinValue (#1953).
                sema::TypeRef tmp = *rt;
                tmp.nullable = true;
                auto* nst = nullableValueStructTy(&tmp);
                auto* valPhi = builder_.CreatePHI(valTy, 2);
                valPhi->addIncoming(loaded, loadBB);
                valPhi->addIncoming(llvm::Constant::getNullValue(valTy), preBB);
                auto* hvPhi = builder_.CreatePHI(nullablePrimHasValTy(), 2);
                hvPhi->addIncoming(
                    llvm::ConstantInt::get(nullablePrimHasValTy(), 1), loadBB);
                hvPhi->addIncoming(
                    llvm::ConstantInt::get(nullablePrimHasValTy(), 0), preBB);
                releaseFreshBoxTemp();  // fix #2865 — after the PHIs
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst), hvPhi, {0u});
                return builder_.CreateInsertValue(res, valPhi, {1u});
            }
            // Other boxable value kinds (enum) — unboxed bare value on success;
            // zeroed on a failed cast (the nullable-enum tag form via `as` is a
            // separate item).
            auto* recPhi = builder_.CreatePHI(valTy, 2);
            recPhi->addIncoming(loaded, loadBB);
            recPhi->addIncoming(llvm::Constant::getNullValue(valTy), preBB);
            releaseFreshBoxTemp();  // fix #2865 — after the PHI
            return recPhi;
        }
        return builder_.CreateSelect(
            isA, obj, llvm::ConstantPointerNull::get(ptrTy()));
    }
    return nullptr;
}


llvm::Value* Emitter::emitIsA(llvm::Value* obj, llvm::GlobalVariable* targetDesc) {
    auto* nullBB = llvm::BasicBlock::Create(llvm_, "isa.null", currentFn_);
    auto* loadBB = llvm::BasicBlock::Create(llvm_, "isa.load", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "isa.body", currentFn_);
    auto* matchBB = llvm::BasicBlock::Create(llvm_, "isa.match", currentFn_);
    auto* nextBB = llvm::BasicBlock::Create(llvm_, "isa.next", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "isa.end", currentFn_);

    llvm::Value* objIsNull = builder_.CreateICmpEQ(
        obj, llvm::ConstantPointerNull::get(ptrTy()));
    builder_.CreateCondBr(objIsNull, nullBB, loadBB);

    builder_.SetInsertPoint(nullBB);
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(loadBB);
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* desc = builder_.CreateLoad(ptrTy(), vptr);  // vtable[0]
    builder_.CreateBr(bodyBB);

    builder_.SetInsertPoint(bodyBB);
    auto* idx = builder_.CreatePHI(i32(), 2);
    idx->addIncoming(builder_.getInt32(0), loadBB);
    llvm::Value* elem = builder_.CreateLoad(
        ptrTy(), builder_.CreateGEP(ptrTy(), desc, {idx}));
    llvm::Value* atNull = builder_.CreateICmpEQ(
        elem, llvm::ConstantPointerNull::get(ptrTy()));
    builder_.CreateCondBr(atNull, endBB, matchBB);

    builder_.SetInsertPoint(matchBB);
    llvm::Value* match = builder_.CreateICmpEQ(elem, targetDesc);
    builder_.CreateCondBr(match, endBB, nextBB);

    builder_.SetInsertPoint(nextBB);
    idx->addIncoming(builder_.CreateAdd(idx, builder_.getInt32(1)), nextBB);
    builder_.CreateBr(bodyBB);

    builder_.SetInsertPoint(endBB);
    auto* result = builder_.CreatePHI(i1(), 3);
    result->addIncoming(builder_.getInt1(false), nullBB);
    result->addIncoming(builder_.getInt1(false), bodyBB);    // hit null
    result->addIncoming(builder_.getInt1(true), matchBB);    // matched
    return result;
}


// §8.6: box an already-lowered value `value` (static type `srcTy`) into a heap
// wrapper `{ boxVtable*, i64 rc, value-bytes }` and return the box pointer. The
// leading vtable word lets `emitIsA` and virtual dispatch recognize the box
// uniformly with a class instance; the #422 rc word (offset 8, BZ_BOX_RC_OFFSET)
// is the producer's reference (initialized to 1). The payload follows at offset
// 16 (two words in) — every payload accessor (the `as` unbox load above, the
// interface-dispatch box thunk in setupBoxDescriptors) advances two words. A
// nullable value type whose source is null boxes to a null Object reference
// (§8.6 Nullable value-type boxing). Boxed values are runtime-managed (§28.9):
// this is a codegen-internal +1 PRODUCER. The box is released at scope end by a
// conservative, escape-proven placement (lowerLocalDecl + emitDisposals) so that
// only a box codegen knows is uniquely held is freed — never a class instance
// (a class coerced to Object keeps its bare pointer and never reaches boxValue),
// and an escaping box is conservatively LEFT leaked rather than double-freed.
llvm::Value* Emitter::boxValue(llvm::Value* value, const sema::TypeRef* srcTy) {
    if (!value || !srcTy) return value;
    // A nullable value type: strip the wrapper (§8.6). null → null Object;
    // non-null → box the underlying value. The lowered value is the tagged
    // {i8 hasValue; T value} struct.
    bool nullableValue = isNullableValueTy(srcTy);
    if (!nullableValue && !isBoxableValueType(srcTy)) return value;  // ref: no box
    llvm::GlobalVariable* vt = boxVtableFor(srcTy);
    if (!vt) return value;  // no descriptor (e.g. unsupported value kind)

    llvm::Value* hasValue = nullptr;
    llvm::Value* payload = value;
    llvm::Type* payloadTy = value->getType();
    if (nullableValue) {
        // {i8 hasValue, T value} → hasValue bit + the stripped T value.
        hasValue = builder_.CreateICmpNE(
            builder_.CreateExtractValue(value, {0u}),
            llvm::ConstantInt::get(nullablePrimHasValTy(), 0), "box.hasval");
        payload = builder_.CreateExtractValue(value, {1u}, "box.payload");
        payloadTy = payload->getType();
    }

    // The box struct { vtable*, i64 rc, payload }. The rc word (#422) sits at a
    // FIXED offset 8 between the vtable word and the payload, so bz_box_release's
    // single runtime offset works for every box regardless of payload size; the
    // payload then lives two words in. Use calloc so any tail padding is zeroed
    // (deterministic box bytes).
    auto* boxTy = llvm::StructType::get(llvm_, {ptrTy(), i64(), payloadTy});
    uint64_t sz = module_->getDataLayout().getTypeAllocSize(boxTy);
    auto emitBox = [&]() -> llvm::Value* {
        llvm::Value* box = builder_.CreateCall(
            callocFn(), {builder_.getInt64(1), builder_.getInt64((int64_t)sz)});
        builder_.CreateStore(vt, box);  // vtable* at offset 0
        // #422 / §28.9: the producer's reference — rc := 1 at offset 8 (the box
        // is a +1 producer, released at scope end when proven non-escaping).
        llvm::Value* rcPtr = builder_.CreateGEP(
            i64(), box, {builder_.getInt32(1)}, "box.rcptr");
        builder_.CreateStore(builder_.getInt64(1), rcPtr);
        // value bytes after the vtable + rc words (offset 16, two words in).
        llvm::Value* pptr = builder_.CreateGEP(
            ptrTy(), box, {builder_.getInt32(2)}, "box.valptr");
        builder_.CreateStore(payload, pptr);
        return box;
    };

    if (!nullableValue) {
        // fix #2865 (§8.6/§28.9/§28.3, v0.240): record the mint so a consumer
        // that provably DISCARDS it can reclaim it — see `lastBoxMint_`'s note
        // in LlvmBackend_impl.hpp and its one reader, lowerTypeTest.
        llvm::Value* b = emitBox();
        lastBoxMint_ = b;
        return b;
    }

    // Nullable source: branch on hasValue — box only when present, else null.
    auto* boxBB = llvm::BasicBlock::Create(llvm_, "box.some", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "box.cont", currentFn_);
    llvm::BasicBlock* preBB = builder_.GetInsertBlock();
    builder_.CreateCondBr(hasValue, boxBB, contBB);
    builder_.SetInsertPoint(boxBB);
    llvm::Value* boxed = emitBox();
    llvm::BasicBlock* boxEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
    auto* phi = builder_.CreatePHI(ptrTy(), 2);
    phi->addIncoming(boxed, boxEndBB);
    phi->addIncoming(llvm::ConstantPointerNull::get(ptrTy()), preBB);
    // fix #2865: the nullable arm's mint is the PHI (null when the source was
    // absent — bz_box_release is null-safe, so the reader needs no extra guard).
    lastBoxMint_ = phi;
    return phi;
}


// §8.6: box `v` (the lowered value of `init`) when the declared location
// `declTy` is Object/interface and the source expression is a value type.
// A no-op for reference sources and non-Object/interface targets — the #399
// reference-type Object path (a class pointer coerced to Object stays a bare
// pointer) is preserved by only boxing boxable VALUE sources.
llvm::Value* Emitter::boxForTarget(llvm::Value* v, const sema::TypeRef* declTy,
                                   const ast::Expr* init) {
    // ⚠ MERGE NOTE (#2901 + #2915): the opening guard deliberately does NOT
    // test `!v`. #2915 needs a null `v` to reach the unit rescue below (unit
    // lowers to LLVM `void`, so `lowerExpr` returns nullptr for `()`), and that
    // rescue is placed BEFORE this function's own `!v` bail for exactly that
    // reason. #2901's type-parameter resolution below is null-`v`-safe: it
    // reads only `declTy`.
    if (!declTy || !init) return v;
    // fix #2901 (§8.6 / §14.1, v0.240): resolve a bare TYPE-PARAMETER target
    // through the active monomorphization before classifying it. §8.6 states,
    // verbatim: "When a value type is used where `Object` (or an interface
    // implemented by the value type) is expected, the value is **boxed**: a
    // heap-allocated wrapper holds the value. Boxed values are runtime-managed
    // ([§28.9])." Under `Nm<IArea>(rb)` the formal `v: T` IS "an interface
    // implemented by the value type" — but only after substitution. Before it,
    // `declTy` is the bare `T` ref (Named, `declaration == nullptr`), which
    // `isObjectOrInterfaceTarget` cannot classify, so it answered "not an
    // Object/interface target" and the box was skipped for EVERY generic
    // parameter at EVERY call site.
    //
    // This is the exact mirror of what #1279 already did on the SOURCE side a
    // few lines below (a `T` bound to a value type, returned where `Object` is
    // expected, must be boxed as its concrete type). The two halves were never
    // symmetric: the source got its substitution, the target never did.
    //
    // The gate stays a PROPERTY, not a list: after resolution the same
    // `isObjectOrInterfaceTarget` decides, so nothing is admitted that a
    // written-out `Object`/interface target would not admit. A bare `T` bound
    // to a value type, a class, or nothing at all resolves to something that
    // predicate still rejects, leaving those paths byte-identical.
    if (!isObjectOrInterfaceTarget(declTy)) {
        const sema::TypeRef* concreteTarget = resolveTypeParamRef(declTy);
        if (concreteTarget == declTy ||
            !isObjectOrInterfaceTarget(concreteTarget))
            return v;
        declTy = concreteTarget;
    }
    const sema::TypeRef* srcTy = ctx_.exprType(*init);
    if (!srcTy) return v;
    // fix #2915 (§4.7 / §8.6 "Boxing of the unit value", v0.240): the unit
    // source is the one boxable kind with NO lowered value — `()` lowers to
    // LLVM `void`, so `lowerExpr` returns nullptr for it and every caller's
    // `if (v)` guard skipped the box, leaving the `Object` slot zero — the NULL
    // reference. §4.7 is explicit that `var o: Object := ();` "produces a boxed
    // `()`", and §8.1 lists the unit type among the kinds that derive from
    // Object via boxing; neither reading §8.6 permits (interned or fresh) is
    // satisfiable by null. Handled BEFORE the `!v` bail below for exactly that
    // reason. Callers that already had a value are untouched.
    //
    // ⚠ The `!v` in this condition is load-bearing, not defensive. Several
    // positions call boxForTarget TWICE on the same argument (the arm that
    // rescues the null value, then the shared boxing site downstream); without
    // it the second call minted a SECOND wrapper and the first leaked —
    // measured under LSan as 48 bytes in 2 allocations for `p := ();` and for
    // `Take(())`, against 24/1 and 0/0 for the same shapes with an `int`. With
    // it, the second call reaches the "already a pointer" bail below and the
    // count matches the int baseline exactly.
    if (!v && srcTy->kind == sema::TypeRef::Kind::Unit && !srcTy->nullable)
        return boxUnitValue();
    if (!v) return v;
    // #1279 / §8.6 / §14.1: a generic value-type parameter `T` / `T?` (declared
    // `where T: value`) returned where `Object`/`Object?` is expected must be
    // boxed like its CONCRETE monomorphized type. Under an active
    // monomorphization the source's sema type is still the bare type-parameter
    // ref (declaration == null), which isBoxableValueType / isNullableValueTy do
    // not recognise as a value type — so the box was skipped and the raw
    // `{i8, T}` / `T` value was `ret`'d into the `ptr` (Object) return (IR-verify
    // failure). Resolve the parameter to its concrete value type first (reusing
    // the #567/#568 substitution helpers), then box that.
    sema::TypeRef concrHolder;
    if (srcTy->kind == sema::TypeRef::Kind::Named && !srcTy->declaration) {
        if (srcTy->nullable) {
            if (nullableTypeParamConcrete(srcTy, concrHolder)) srcTy = &concrHolder;
        } else if (const sema::TypeRef* c = resolveTypeParamRef(srcTy);
                   c && c != srcTy) {
            srcTy = c;
        }
    }
    // Only box a value-typed source. A reference source (class/interface/
    // string/Object) already lowers to a pointer and reaches Object directly.
    if (!isBoxableValueType(srcTy) && !isNullableValueTy(srcTy)) return v;
    // The lowered value must be a scalar/struct (a value), not already a ptr
    // (which would mean it was lowered as a reference — leave it alone).
    if (v->getType()->isPointerTy() && !isNullableValueTy(srcTy)) return v;
    return boxValue(v, srcTy);
}


// fix #3090 (§12.7 / §8.6, v0.252). See the declaration comment in
// LlvmBackend_impl.hpp for the mechanism and why the gate is a positive
// conjunction.
const sema::TypeRef* Emitter::narrowedIdentDeclaredType(const ast::Expr& e) {
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e);
    if (!id || id->name == "this" || id->name == "base") return nullptr;
    sema::Symbol* sym = ctx_.exprSymbol(e);
    if (!sym) return nullptr;
    // §12.7 narrows only SIMPLE VARIABLES — "Narrowing applies to the original
    // variable" — which sema keys by LocalSymbol / ParameterSymbol
    // (narrowFromTypeTest). Anything else (a field, a property, a static) is not
    // a narrowing site at all, so it is not a narrowed read either.
    const sema::TypeRef* declTy = nullptr;
    if (auto* ls = dynamic_cast<const sema::LocalSymbol*>(sym)) {
        declTy = ls->declType;
    } else if (auto* ps = dynamic_cast<const sema::ParameterSymbol*>(sym)) {
        declTy = ps->resolvedDeclType;
        if (!declTy && ps->declType) declTy = ctx_.resolvedType(*ps->declType);
    } else {
        return nullptr;
    }
    // Only an `Object`/interface DECLARED location holds a §8.6 box (or, for a
    // string/class source, the bare pointer that IS the value — #399/#1071). A
    // local declared at the value type itself already stores the value directly.
    if (!isObjectOrInterfaceTarget(declTy)) return nullptr;
    const sema::TypeRef* nt = ctx_.exprType(e);
    if (!nt || nt == declTy) return nullptr;
    // Narrowing from `Object` to an INTERFACE leaves the operand classified as a
    // reference either way, so there is nothing to correct and the published type
    // is the more precise one — leave those callers reading it.
    if (isObjectOrInterfaceTarget(nt)) return nullptr;
    return declTy;
}


const sema::TypeRef* Emitter::narrowedBoxedValueTypeOf(const ast::Expr& e) {
    if (narrowedUnboxSuppressed_) return nullptr;  // see the flag's comment
    const sema::TypeRef* declTy = narrowedIdentDeclaredType(e);
    if (!declTy) return nullptr;
    const sema::TypeRef* nt = ctx_.exprType(e);
    if (!nt) return nullptr;
    // A §12.7 narrowing is non-nullable by construction (narrowFromTypeTest
    // strips it): a nullable published type here is some OTHER rule's, not this
    // one's, so decline rather than guess.
    if (nt->nullable) return nullptr;
    // §4.7/§8.6: the unit type lowers to LLVM `void`, so there is no value to
    // load — the same exclusion #2915 made on the `as` unbox arm. Sema does not
    // narrow to it either (narrowFromTypeTest's positive list), so this is a
    // belt-and-braces guard on an unreachable shape rather than a live arm.
    if (nt->kind == sema::TypeRef::Kind::Unit) return nullptr;
    if (!isBoxableValueType(nt)) return nullptr;
    return nt;
}


// fix #3090 (§12.7 / §8.6, v0.252). See the declaration comment.
llvm::Value* Emitter::narrowedBoxPayloadAddr(const ast::Expr& e,
                                             llvm::Type*& elemTy) {
    const sema::TypeRef* nt = narrowedBoxedValueTypeOf(e);
    if (!nt) return nullptr;
    llvm::Type* payloadTy = lowerTypeRef(nt);
    if (!payloadTy || payloadTy->isVoidTy()) return nullptr;
    // The STORAGE still holds the `Object` reference, so read the box pointer
    // out of the slot exactly as the un-narrowed read would. Deliberately does
    // NOT go through lvalueAddr — that function calls this one.
    llvm::Type* slotTy = nullptr;
    llvm::Value* slotAddr = refParamAddr(e, slotTy);
    if (!slotAddr) {
        if (llvm::AllocaInst* slot = localSlot(e))
            slotAddr = boxedStorage(slot, slotTy);  // §27.5 capture cell
    }
    if (!slotAddr || !slotTy || !slotTy->isPointerTy()) return nullptr;
    llvm::Value* boxPtr = builder_.CreateLoad(ptrTy(), slotAddr, "narrow.box");
    // Payload two words in — past the vtable word and the #422 rc word, the
    // same `{ vtable*, i64 rc, payload }` layout boxValue writes and the §17.4
    // `as` arm reads.
    elemTy = payloadTy;
    return builder_.CreateGEP(ptrTy(), boxPtr, {builder_.getInt32(2)},
                              "narrow.payload");
}


// fix #3090 (§12.7 / §8.6, v0.252). See the declaration comment.
llvm::Value* Emitter::unboxNarrowedRead(const ast::Expr& e) {
    llvm::Type* payloadTy = nullptr;
    llvm::Value* p = narrowedBoxPayloadAddr(e, payloadTy);
    if (!p || !payloadTy) return nullptr;
    return builder_.CreateLoad(payloadTy, p, "narrow.unbox");
}

// fix #3095 (§4.7, v0.252): the `()?` sibling of boxForTarget's unit rescue.
//
// A unit-typed source expression has NO lowered value — `()` lowers to LLVM
// `void`, so `lowerExpr` returns nullptr for it (#2915). At a `()?` LOCATION
// that nothing must become the PRESENT optional `{i8 1, i8 0}`: §4.7 gives
// `()?` "an optional with two values, `null` and `()`", and leaving the slot as
// it was (the pre-fix behaviour) collapses the two — MEASURED on 663a68e,
// `var u: ()? := null; u := ();` left `u` reading as `null`, behind the #2961
// "store dropped" Warning.
//
// Called from the SAME four value-demanding positions #2915 enumerates — local
// declaration, assignment, call argument, `return` — immediately alongside the
// `boxForTarget` call each of them already makes, so the two rescues agree by
// construction. It is a no-op whenever a value WAS lowered, whenever the target
// is not `()?`, and whenever the source is not the non-nullable unit value
// (a `()?`-typed source already carries its own tag).
// fix #3421 (§4.7, v0.283): this predicate now answers for the PLAIN `()`
// target as well, making it the ONE place that says what a unit source lowers
// to at a value-demanding location. Its call sites in argument position each
// carried their own copy of that second arm — the duplicated-classification
// shape #3419 tracks, and the reason #2915/#3095/#3105/#3421 were four
// separately-filed bugs over one root cause.
//
// The placeholder byte is the same one `lowerParamType` and `lowerFieldType`
// substitute for the `void` a unit parameter or field would otherwise lower to,
// and the same one `lowerLocalDecl`'s #1619 slot swap uses: `void` is not a
// valid parameter, field, or `alloca` type. Its CONTENTS are never meaningfully
// read — §4.7 gives `()` "exactly one value", so no byte pattern is
// distinguishable from another — it exists only to keep arity and types
// matching at a location whose signature has a slot for it.
llvm::Value* Emitter::unitValueForTarget(llvm::Value* v,
                                         const sema::TypeRef* declTy,
                                         const ast::Expr* init) {
    if (v || !declTy || !init) return v;
    const bool nullableTarget = isNullableUnitTy(declTy);
    const bool unitTarget =
        declTy->kind == sema::TypeRef::Kind::Unit && !declTy->nullable;
    if (!nullableTarget && !unitTarget) return v;
    const sema::TypeRef* srcTy = ctx_.exprType(*init);
    if (!srcTy || srcTy->kind != sema::TypeRef::Kind::Unit || srcTy->nullable)
        return v;
    if (unitTarget) return llvm::ConstantInt::get(unitPlaceholderTy(), 0);
    return nullableUnitPresent(declTy);
}


// fix #3433 (§4.7 / §12.8 "Constructor chaining" / §22.7 "Constructors" /
// §22.8, v0.283): the
// constructor-chain face of `unitValueForTarget` — see the declaration comment.
//
// §4.7 makes a method declared without a return type a producer of the unit
// value, and "The unit type is a value type"; §22.7 places no restriction on
// the argument types a chain call may carry. `new B(C.Nothing())` already
// compiled (that path runs `buildOrderedArgs`, which carries the #2915/#3095/
// #3421 rescue); only the `base(...)`/`this(...)` and `: Base(...)` spellings
// of the SAME argument at the SAME constructor rejected it.
//
// The formal comes from `paramTypeOf` — the existing #1592 authority for "the
// resolved declared TypeRef of a formal", which prefers an imported symbol's
// `resolvedDeclType` and falls back to resolving a local one's `ast::Type`, so
// an imported chain target answers as well as a local one. `concr` is kept
// alive across the `unitValueForTarget` call deliberately: it is a
// caller-owned holder, not a cached TypeRef.
//
// ⚠ THE GENERIC ARM IS NOT REACHED — an earlier revision of this comment
// claimed it was. A generic formal monomorphized to `()`/`()?` (§7.10's
// `where T: value` admitted substitutions) arrives as a bare `Named T`, and
// this code does try to resolve it through the ACTIVE substitution — but
// `genericSubst_` is EMPTY here. The argument loop is `Setup.cpp:11497`,
// while the base-class substitution (`T -> ()`, the #617 mechanism) is built
// and activated at `:11560`-`:11593`, i.e. AFTER the loop has run. So
// `resolveTypeParamRef` is a no-op and the formal stays a bare `Named T`.
// MEASURED, identical on this tree and on 0c5ecd1:
//     class D: B<()>   … base(C.Nothing());   -> #1386 at :7:10
//     class D(): B<()>(C.Nothing())           -> #1386 at :7:18
// No test covers that arm. Tracked as issue 3501, which also records that the
// `new B<()>(C.Nothing())` spelling is broken too — a whole missing family
// rather than a gap in this fix. Do not read the paragraph above as covering
// it.
llvm::Value* Emitter::unitValueForCtorParam(llvm::Value* v,
                                            sema::ConstructorSymbol* target,
                                            std::size_t idx,
                                            const ast::Expr* arg) {
    if (v || !target || !arg || idx >= target->params.size()) return v;
    sema::TypeRef* formal = paramTypeOf(target->params[idx]);
    if (!formal) return v;
    if (formal->kind == sema::TypeRef::Kind::Named && !formal->declaration &&
        formal->nullable) {
        sema::TypeRef concr;
        if (nullableTypeParamConcrete(formal, concr))
            return unitValueForTarget(v, &concr, arg);
        return unitValueForTarget(v, formal, arg);
    }
    return unitValueForTarget(v, resolveTypeParamRef(formal), arg);
}


llvm::Value* Emitter::refParamAddr(const ast::Expr& e, llvm::Type*& elemTy) {
    if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
    sema::Symbol* sym = ctx_.exprSymbol(e);
    if (!sym) return nullptr;
    auto it = refParams_.find(sym);
    if (it == refParams_.end()) return nullptr;
    elemTy = it->second.second;
    return it->second.first;
}


llvm::AllocaInst* Emitter::localSlot(const ast::Expr& e) {
    if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
    sema::Symbol* sym = ctx_.exprSymbol(e);
    if (!sym) return nullptr;
    if (dynamic_cast<sema::ParameterSymbol*>(sym)) {
        auto it = paramSlots_.find(sym);
        return it == paramSlots_.end() ? nullptr : it->second;
    }
    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
        if (!loc->decl) return nullptr;
        auto it = locals_.find(loc->decl);
        return it == locals_.end() ? nullptr : it->second;
    }
    return nullptr;
}


// §27.5: route a local/param access through its shared heap cell when the
// variable has been boxed for capture; otherwise use the alloca directly.
//
// fix #3484 (§27.5, v0.283): the cell-holder alloca is NOT valid on every path
// that reaches a read. boxLocalSlot's GUARDED strategy deliberately makes the
// holder null-checkable — it null-initialises it in the entry block and
// allocates the cell behind a `cell.isnull` branch emitted AT THE CAPTURE SITE.
// When that capture site sits in a conditionally-executed block (a `case` arm,
// an `if` body, an if-EXPRESSION arm, a loop that may run zero times), the
// not-taken path never runs the allocation, so the holder is still the entry
// block's null when a LATER read of the same variable loads it — and this
// function handed that null straight back as the address to load from
// (`SEGV on unknown address 0x0`, measured on six spellings — a `case` arm with
// and without a `do … end`, an `if` body, an if-EXPRESSION arm, and a zero-trip
// `while` with either an ordinary read or a `using var`'s scope-end DISPOSAL as
// the surviving reader — over a captured parameter and a captured local alike,
// purely synchronous, with no `await` and no unsafe construct in the program).
//
// WHICH HALF IS WRONG is decided by the design of the holder, not by taste: the
// guarded strategy is what makes the holder nullable in the first place (that
// null is how the runtime tells a first pass from a later one across a loop
// back-edge), so the holder is CORRECT and the reads are the wrong half. Repair
// them here, at the single funnel every boxed read and write already goes
// through, rather than growing a second null-discipline at ~40 call sites.
//
// The repair is NOT emitted inline, because whether a given read is exposed
// depends on the FINISHED CFG — a read the capture site's join block dominates
// can never see the null — and at the moment a read is emitted the blocks and
// edges above it do not all exist yet. So the read is recorded here and patched
// once, from run(), by fixupCapturedCellHolderReads below. Emitting the guard
// unconditionally would be both dead code on the dominated reads and an
// observable perturbation: see cellHolderReadyBlocks_'s declaration comment for
// the five one-shot LeakSanitizer rows that measured it.
llvm::Value* Emitter::boxedStorage(llvm::AllocaInst* slot, llvm::Type*& elemTy) {
    auto it = cellVars_.find(slot);
    if (it != cellVars_.end()) {
        elemTy = it->second;
        // The alloca holds a pointer to the heap cell; the value lives there.
        llvm::Value* cell = builder_.CreateLoad(ptrTy(), slot, "cell");
        // Only a GUARDED/fresh boxing of THIS emission records a sync slot, and
        // it records it in the same breath as cellVars_[slot] (boxLocalSlot's
        // tail), so a cellVars_ hit current enough for the load above makes the
        // paired sync entry current too. A cell holder with no sync entry — an
        // async-lambda capture restored from its env, a captured ref/out param
        // aliasing the caller's storage — is not this defect's shape and is left
        // exactly as it was.
        auto syncIt = cellVarSyncSlot_.find(slot);
        if (syncIt != cellVarSyncSlot_.end() && syncIt->second) {
            PendingHolderRead p;
            p.load = llvm::WeakTrackingVH(cell);
            p.syncSlot = llvm::WeakTrackingVH(syncIt->second);
            // Snapshot now, while `slot` is certainly live — see the map's
            // declaration comment.
            auto rb = cellHolderReadyBlocks_.find(slot);
            if (rb != cellHolderReadyBlocks_.end()) p.readyBlocks = rb->second;
            // Inside a STATE MACHINE the fall-back slot is only usable if it
            // round-trips the frame. A local's does once lowerLambda has given it
            // its own field; a parameter's does via the resume prologue's
            // unconditional restore, which is why parameters are admitted here
            // whether or not they carry a field of their own.
            //
            // ⚠ This sentence used to name "an async LAMBDA's own body local
            // (its frame is hand-rolled and reserves no such field)" as the
            // leading example of what is declined. `fix #3499` reserves that
            // field and publishes it into smAllocaToField_, so an async
            // lambda's body local now ROUND-TRIPS and is admitted here — the
            // predicate below did not change, the world underneath it did.
            //
            // What is still declined: anything the predicate cannot see a field
            // for. A catch-arm binding's holder WAS declined here because its
            // fall-back address (the binding's OWN alloca) is moved into the
            // handler SPILL AREA rather than a frame field — so it failed the
            // `frame field` predicate only. `fix #3506` admits it as a third
            // branch of `syncSurvivesSuspend`, recording the fact at read-time
            // while `asyncHandlerSpillSlots_` still names the slot. The fixup
            // side relaxes DECLINE 2 to also accept a `GetElementPtrInst` in
            // the read's own function (the spill-area GEP the demotion RAUWs
            // the alloca into — computed in `entry.spillcont` which dominates
            // every state block). Together the two admits close the
            // conditional-capture-in-awaiting-arm shape (#3506) without
            // widening the predicate generically.
            //
            // The reason to decline when in doubt is unchanged: addressing a
            // slot that did NOT round-trip after a resume turns a loud SIGSEGV
            // into a quiet wrong value, and the read then stays exactly as it
            // was before #3484.
            if (smStateStruct_ && smSelf_) {
                p.syncSurvivesSuspend =
                    smAllocaToField_.count(syncIt->second) != 0 ||
                    smRestoredParamSlots_.count(syncIt->second) != 0 ||
                    // `fix #3506` (§27.5 / §30.9 / §31.2): a catch-arm binding
                    // whose arm awaits is registered in `asyncHandlerSpillSlots_`
                    // by `lowerTry`; `promoteDemotedSlotsToFrame` then RAUWs
                    // the slot into a GEP into the calloc'd spill area. The
                    // spill area is heap-persistent for the frame's lifetime,
                    // and the GEP is computed in `entry.spillcont` which
                    // dominates every state block — so addressing it reads a
                    // genuine round-tripped value, exactly as the frame-field
                    // branch does. The membership test runs BEFORE
                    // `fixupSuspendDominance` clears the vector, so it sees
                    // the slot the same way `promoteDemotedSlotsToFrame`
                    // will. Admitting here is necessary; the matching relax of
                    // DECLINE 2 below is also necessary.
                    std::find(asyncHandlerSpillSlots_.begin(),
                              asyncHandlerSpillSlots_.end(),
                              syncIt->second) !=
                        asyncHandlerSpillSlots_.end();
            }
            pendingHolderReads_.push_back(std::move(p));
        }
        return cell;
    }
    elemTy = slot->getAllocatedType();
    return slot;
}


// fix #3484 (§27.5, v0.283): give every EXPOSED capture-cell holder read a null
// fallback, and leave every dominated one untouched. Run once from run(), after
// every function body is complete, because the dominance question is about the
// finished CFG (see boxedStorage).
//
// The fallback address is the PRE-BOXING alloca recorded in cellVarSyncSlot_ —
// exactly the storage boxLocalSlot itself reads to seed the cell if the capture
// site is reached later on. So the fallback is not an approximation:
//   * holder null  ⇒ no closure over this variable exists yet, nothing else can
//                    observe it, and the original slot IS its storage; reads see
//                    the live value and writes land where a later `cell.alloc`
//                    will seed itself from (its `preboxed` load);
//   * holder set   ⇒ the select folds to the cell and §27.5 sharing is exactly
//                    as before.
//
// DIRECTION OF DANGER: this widens no free and shortens no lifetime. It turns a
// null dereference into a load/store of storage that lives as long as the frame;
// a cell that is never allocated was already never freed (`free(null)` is a
// no-op), so nothing here can double-free or free early.
void Emitter::fixupCapturedCellHolderReads() {
    if (pendingHolderReads_.empty()) return;
    // Group by function so one DominatorTree serves every read in it.
    std::unordered_map<llvm::Function*, std::vector<const PendingHolderRead*>>
        byFn;
    for (const PendingHolderRead& p : pendingHolderReads_) {
        // DECLINE 1 — the read itself is gone. fixupSuspendDominance's
        // DemoteRegToStack erases the registers it demotes, and a resume
        // function's holder load is a candidate.
        auto* ld = llvm::dyn_cast_or_null<llvm::Instruction>(
            static_cast<llvm::Value*>(p.load));
        if (!ld || !ld->getParent() || !ld->getFunction()) continue;
        // DECLINE 2 — the fall-back slot is gone, has been moved out of this
        // function, or is now a value the lowered read cannot address.
        // Originally: "must be an AllocaInst of this function" — that is what
        // `fix #3484 (follow-up)`'s D2 needed because the original alloca's
        // memory was being recycled into another function's alloca, producing
        // malformed IR. `fix #3506` widens the ADMISSION to also accept a
        // `GetElementPtrInst` in the read's own function: that is the
        // post-RAUW form of a slot in `asyncHandlerSpillSlots_`, computed in
        // `entry.spillcont` which dominates every state block, and whose
        // pointee is the heap-persistent spill area. `WeakTrackingVH` still
        // nulls on true deletion, so a destroyed GEP still declines.
        // Anything else (a constant, a phi, an unrelated instruction) is
        // declined exactly as before.
        auto* syncInst = llvm::dyn_cast_or_null<llvm::Instruction>(
            static_cast<llvm::Value*>(p.syncSlot));
        if (!syncInst || !syncInst->getParent() ||
            syncInst->getFunction() != ld->getFunction()) continue;
        if (!llvm::isa<llvm::AllocaInst>(syncInst) &&
            !llvm::isa<llvm::GetElementPtrInst>(syncInst)) continue;
        // DECLINE 3 — inside a state machine, the fall-back slot is not restored
        // across a suspension, so addressing it after a resume would read a
        // fresh-stack alloca: a plausible WRONG VALUE in place of a loud SIGSEGV,
        // which is strictly worse. Leave the read exactly as it was.
        if (!p.syncSurvivesSuspend) continue;
        byFn[ld->getFunction()].push_back(&p);
    }
    for (auto& [fn, reads] : byFn) {
        if (!fn || fn->isDeclaration()) continue;
        llvm::DominatorTree dt(*fn);
        for (const PendingHolderRead* p : reads) {
            auto* ld = llvm::cast<llvm::Instruction>(
                static_cast<llvm::Value*>(p->load));
            // `sync` may be the original `AllocaInst` or its post-RAUW GEP
            // form (the matched DECLINE 2 above already accepted both); an
            // `Instruction` is sufficient here because we only pass it as a
            // pointer-typed operand to `CreateSelect`.
            auto* sync = llvm::cast<llvm::Instruction>(
                static_cast<llvm::Value*>(p->syncSlot));
            // Dominated by any completion point of this holder's boxing ⇒ the
            // cell is allocated on every path that reaches this read. Leave the
            // IR untouched — that bit-identity is what keeps a program with an
            // unconditional capture out of this fix entirely.
            bool ready = false;
            for (llvm::BasicBlock* b : p->readyBlocks) {
                if (!b || b->getParent() != fn) continue;
                if (dt.dominates(b, ld->getParent())) { ready = true; break; }
            }
            if (ready) continue;
            llvm::IRBuilder<> b(ld->getParent(), std::next(ld->getIterator()));
            llvm::Value* isNull = b.CreateICmpEQ(
                ld, llvm::ConstantPointerNull::get(ptrTy()), "cell.unset");
            llvm::Value* sel = b.CreateSelect(isNull, sync, ld, "cell.orslot");
            ld->replaceAllUsesWith(sel);
            // RAUW also rewrote the two uses INSIDE the guard it just built,
            // which would make the select its own operand. Point them back.
            llvm::cast<llvm::Instruction>(isNull)->setOperand(0, ld);
            llvm::cast<llvm::Instruction>(sel)->setOperand(2, ld);
        }
    }
    pendingHolderReads_.clear();
}


// §27.5: promote a local/param slot to a shared heap cell so that the
// enclosing function and any capturing closure access the same storage.
// Idempotent: a slot already boxed is reused (multiple lambdas capturing the
// same variable share one cell). Returns the cell pointer and, through
// `newSlot`, the `ptr`-typed alloca that now holds it.
//
// BUG-L2 / §27.5: `forLoopVar` selects the boxing strategy. When false
// (regular local/param), a single shared cell is allocated and reused across
// all loop iterations via a null-guard — the cell is not re-allocated on each
// iteration so accumulated writes are never discarded. When true (for-var loop
// variable), a fresh cell is created on every pass (the for-loop incBB keeps
// the original slot current, so each pass reads the correct iteration value).
llvm::Value* Emitter::boxLocalSlot(llvm::AllocaInst* slot,
                                   llvm::AllocaInst*& newSlot,
                                   bool forLoopVar,
                                   bool* wasGuardedBoxing) {
    auto it = cellVars_.find(slot);
    if (it != cellVars_.end()) {
        // Already boxed: the slot is already a ptr-alloca holding the cell.
        //
        // fix #2303 / §27.5: the cell allocation is emitted at the FIRST
        // boxLocalSlot call site, behind a null-check guard. A SUBSEQUENT call
        // (a second lambda capturing the same local) used to return the slot
        // value directly, assuming the first call's alloc had run. That is
        // sound only when the first call's basic block is always reached at
        // runtime — which is NOT the case when the first lambda lives behind
        // a conditional the runtime did not take. The second lambda then
        // stored the entry-block zero into its env, and the body dereferenced
        // address 0 → SEGV.
        //
        // Emit the guard HERE too: null-check the cell-holder, and if it is
        // still null (first alloc was skipped), allocate now from the
        // original-slot sync entry. The cell is then guaranteed to exist
        // before any lambda uses it, regardless of which capture path the
        // runtime took. The branch is predictable (non-null after the first
        // lambda runs), so the cost is one load + compare + predictable
        // branch per capture — well within the noise floor.
        //
        // `wasGuardedBoxing` stays FALSE: the cell was already allocated by
        // the first call (the caller is the SECOND+ capture, and the lower
        // lambda's additional-capture retain for strings/closures — #672,
        // lines ~1365-1376 — must still fire to balance the N envdtor
        // releases; that retain is gated on `!wasGuardedLocal`). Setting the
        // flag true here would suppress the retain → over-release / double
        // free on multi-lambda string/closure captures (the
        // `spec_27_5_28_9_672_string_two_lambda_capture_no_uaf` and three-
        // lambda siblings, which were green on base). The cell-free / box-
        // release branches the flag would otherwise gate are all in the
        // `!alreadyBoxed` path and are skipped for this call regardless.
        newSlot = slot;
        if (wasGuardedBoxing) *wasGuardedBoxing = false;

        llvm::Type* valTy = it->second;
        llvm::Function* fn = currentFn_;
        auto* allocBB = llvm::BasicBlock::Create(llvm_, "cell.alloc", fn);
        auto* doneBB  = llvm::BasicBlock::Create(llvm_, "cell.done",  fn);

        llvm::Value* existingCell = builder_.CreateLoad(
            ptrTy(), newSlot, "cell.existing");
        llvm::Value* isNull = builder_.CreateICmpEQ(
            existingCell, llvm::ConstantPointerNull::get(ptrTy()),
            "cell.isnull");
        builder_.CreateCondBr(isNull, allocBB, doneBB);

        // Allocation branch: first pass (or previous alloc was skipped).
        builder_.SetInsertPoint(allocBB);
        llvm::Value* cellSize = builder_.CreatePtrToInt(
            builder_.CreateGEP(valTy,
                               llvm::ConstantPointerNull::get(ptrTy()),
                               {builder_.getInt32(1)}),
            i64());
        llvm::Value* cell = builder_.CreateCall(
            callocFn(), {builder_.getInt64(1), cellSize});
        // Read the original slot's value (tracked in cellVarSyncSlot_) to
        // initialise the cell. If the sync entry is missing for any reason,
        // the cell stays at its calloc zero (the safe default for a value
        // type — the lambda will read 0, which is observably wrong but
        // never a crash; the spec's normal-edge release + the envdtor's
        // null-safe release keep the bookkeeping sound).
        auto syncIt = cellVarSyncSlot_.find(newSlot);
        if (syncIt != cellVarSyncSlot_.end()) {
            llvm::Value* cur = builder_.CreateLoad(
                valTy, syncIt->second, "preboxed");
            builder_.CreateStore(cur, cell);
        }
        builder_.CreateStore(cell, newSlot);
        builder_.CreateBr(doneBB);

        // Done: cell ptr is in *newSlot.
        builder_.SetInsertPoint(doneBB);
        // fix #3484: from this join on, the holder is non-null on every path
        // through it — the guard above allocated it if it was not. Recorded so
        // fixupCapturedCellHolderReads can leave every read this block dominates
        // exactly as emitted.
        cellHolderReadyBlocks_[newSlot].push_back(doneBB);
        return builder_.CreateLoad(ptrTy(), newSlot, "cell");
    }
    llvm::Type* valTy = slot->getAllocatedType();

    // Determine whether to use a fresh cell each iteration or a single reused
    // cell (null-guard).  A fresh cell is required when:
    //   (a) the slot is a for-var loop variable (forLoopVar=true), OR
    //   (b) the slot's value is re-initialised each iteration by code outside
    //       the boxing mechanism — detected by checking if any StoreInst that
    //       writes to `slot` is in the current basic block (the loop body).
    //       E.g. `using var r := new R()` inside a while loop re-stores into
    //       r_slot each iteration; using the null-guard would preserve the
    //       first iteration's (disposed) pointer in the cell.
    bool freshEachIteration = forLoopVar;
    // #823 / §27.5: a local declared inside a loop body is re-initialised each
    // iteration → fresh cell each pass. This is robust to nested control flow
    // (an inner loop / if-then) that puts the init store in a different basic
    // block than the capture site, which the current-BB store heuristic below
    // misses (it would then reuse one stale first-iteration cell).
    if (!freshEachIteration && loopBodyDeclaredSlots_.count(slot))
        freshEachIteration = true;
    if (!freshEachIteration) {
        llvm::BasicBlock* cur = builder_.GetInsertBlock();
        for (llvm::Use& u : slot->uses()) {
            if (auto* si = llvm::dyn_cast<llvm::StoreInst>(u.getUser())) {
                if (si->getPointerOperand() == slot && si->getParent() == cur) {
                    freshEachIteration = true;
                    break;
                }
            }
        }
    }

    if (freshEachIteration) {
        // Fresh cell each iteration: allocate and initialise from slot on every
        // pass (for-var loop variable fresh binding, or per-iteration re-init).
        // NOT guarded: a fresh cell is created every time this code runs.
        if (wasGuardedBoxing) *wasGuardedBoxing = false;
        llvm::Value* cellSize = builder_.CreatePtrToInt(
            builder_.CreateGEP(valTy, llvm::ConstantPointerNull::get(ptrTy()),
                               {builder_.getInt32(1)}),
            i64());
        llvm::Value* cell = builder_.CreateCall(callocFn(),
                                                {builder_.getInt64(1), cellSize});
        llvm::Value* cur2 = builder_.CreateLoad(valTy, slot, "preboxed");
        builder_.CreateStore(cur2, cell);
        newSlot = entryAlloca(ptrTy());
        builder_.CreateStore(cell, newSlot);
        cellVars_[newSlot] = valTy;
        // fix #3484: the fresh path stores unconditionally, so the holder is
        // non-null from here on through this block. (No cellVarSyncSlot_ entry
        // is recorded on this path, so boxedStorage records no read for it
        // either; the block is recorded for symmetry and for a slot that later
        // reaches the already-boxed re-guard above.)
        cellHolderReadyBlocks_[newSlot].push_back(builder_.GetInsertBlock());
        return cell;
    }

    // Regular local declared outside the loop: guarded boxing — allocate and
    // initialise the cell exactly once (first pass); reuse it on subsequent
    // passes (loop re-entry) so accumulated writes are never reset from the
    // stale original slot.
    // The cell-holder alloca lives in the entry block and is null-initialised
    // there so the runtime guard can distinguish first vs. subsequent passes.
    // GUARDED: the cell outlives any inner scope (loop body) where it is used.
    // The caller must NOT free this cell at inner-scope exit.
    if (wasGuardedBoxing) *wasGuardedBoxing = true;
    newSlot = entryAlloca(ptrTy());
    {
        llvm::BasicBlock& entry = currentFn_->getEntryBlock();
        auto insertPt = std::next(newSlot->getIterator());
        llvm::IRBuilder<> eb(&entry, insertPt);
        eb.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), newSlot);
    }

    // Runtime guard.
    llvm::Function* fn = currentFn_;
    auto* allocBB = llvm::BasicBlock::Create(llvm_, "cell.alloc", fn);
    auto* doneBB  = llvm::BasicBlock::Create(llvm_, "cell.done",  fn);

    llvm::Value* existingCell =
        builder_.CreateLoad(ptrTy(), newSlot, "cell.existing");
    llvm::Value* isNull = builder_.CreateICmpEQ(
        existingCell, llvm::ConstantPointerNull::get(ptrTy()), "cell.isnull");
    builder_.CreateCondBr(isNull, allocBB, doneBB);

    // Allocation branch: first pass only.
    builder_.SetInsertPoint(allocBB);
    llvm::Value* cellSize = builder_.CreatePtrToInt(
        builder_.CreateGEP(valTy, llvm::ConstantPointerNull::get(ptrTy()),
                           {builder_.getInt32(1)}),
        i64());
    llvm::Value* cell = builder_.CreateCall(callocFn(),
                                            {builder_.getInt64(1), cellSize});
    llvm::Value* cur = builder_.CreateLoad(valTy, slot, "preboxed");
    builder_.CreateStore(cur, cell);
    builder_.CreateStore(cell, newSlot);
    builder_.CreateBr(doneBB);

    // Done: cell ptr is in *newSlot.
    builder_.SetInsertPoint(doneBB);
    llvm::Value* cellPtr = builder_.CreateLoad(ptrTy(), newSlot, "cell");

    // fix #3484 (§27.5, v0.283): this join is where the holder becomes valid.
    // Any read the block DOMINATES is safe as emitted; any read it does not —
    // the whole point of the issue, a capture site inside a `case` arm / `if`
    // body / if-expression arm / zero-trip loop — needs the null fallback
    // fixupCapturedCellHolderReads adds.
    cellHolderReadyBlocks_[newSlot].push_back(doneBB);

    cellVars_[newSlot] = valTy;
    // §27.5 / #673: record the original slot so writes to the cell also
    // sync back to the pre-boxing alloca.  This keeps pre-emitted code
    // (e.g. a while-loop condition already lowered before boxing) in sync.
    cellVarSyncSlot_[newSlot] = slot;
    return cellPtr;
}


// §27.5: rebind a boxed `using var`'s pending disposal to its new slot.
void Emitter::rebindDisposalSlot(llvm::AllocaInst* oldSlot,
                                 llvm::AllocaInst* newSlot) {
    if (oldSlot == newSlot) return;
    for (auto& scope : disposeStack_)
        for (auto& d : scope)
            if (d.slot == oldSlot) d.slot = newSlot;
}


namespace {
// §27.5: recursive capture-set walker. Visits every sub-expression and
// sub-statement of a node, recording the symbol of any identifier reference.
// Nested lambdas are descended into so their captures of an outer scope are
// counted too. The walk over-approximates harmlessly (e.g. it records a
// lambda's own parameters and inner locals as well); the caller intersects
// the result against the enclosing function's known locals/params.
struct CaptureWalker {
    const sema::SemaContext& ctx;
    std::unordered_set<const sema::Symbol*>& out;

    void expr(const ast::Expr* e) {
        if (!e) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            if (sema::Symbol* s = ctx.exprSymbol(*id)) out.insert(s);
            return;
        }
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(m->target.get()); return; }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get()); return; }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            return; }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            return; }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get());
            for (auto& a : ix->indices) expr(a.value.get());
            return; }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get()); expr(b->right.get()); return; }
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get()); return; }
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get()); return; }
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get()); return; }
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get()); expr(c->thenValue.get());
            expr(c->elseValue.get()); return; }
        if (auto* t = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(t->operand.get()); return; }
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(r->lo.get()); expr(r->hi.get()); expr(r->step.get());
            return; }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get());
            return; }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get());
            for (auto& a : w->assigns) expr(a.value.get());
            return; }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get());
            for (auto& k : cl->keys) expr(k.get());
            return; }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : is->parts) expr(part.expr.get());
            return; }
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
            // Descend into nested lambdas: their references reach the
            // grandparent scope too.
            if (lam->exprBody) expr(lam->exprBody.get());
            for (auto& s : lam->stmts) stmt(s.get());
            return; }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get());
            for (auto& arm : ce->arms) {
                expr(arm.guard.get());
                expr(arm.exprBody.get());
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return; }
        // Literals and other leaf expressions: nothing to do.
    }

    void stmt(const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            expr(ld->initializer.get()); return; }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get()); return; }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            expr(r->value.get()); return; }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            expr(es->expr.get()); return; }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get()); return; }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            expr(as->target.get()); expr(as->value.get()); return; }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            expr(inc->target.get()); return; }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            expr(ds->value.get()); return; }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get()); return; }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get());
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get());
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return; }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get());
            for (auto& b : ws->body) stmt(b.get());
            return; }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            expr(fs->iterable.get()); expr(fs->withExpr.get());
            for (auto& b : fs->body) stmt(b.get());
            return; }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return; }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) stmt(b.get());
            for (auto& c : ts->catches) {
                expr(c.guard.get());
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return; }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get());
            for (auto& arm : cs->arms) {
                expr(arm.guard.get());
                expr(arm.exprBody.get());
                for (auto& a : arm.stmts) stmt(a.get());
            }
            return; }
        // BreakStmt / ContinueStmt: no sub-expressions.
    }
};

// §27.5 / #754: collect the OUTERMOST LambdaExpr nodes appearing in a statement
// list, WITHOUT descending into the lambdas themselves. The caller runs
// collectCapturedSyms on each (which DOES descend into nested lambdas), so a
// for-var captured only by a nested lambda is still detected. Mirrors
// CaptureWalker's traversal of the AST, but records lambdas instead of symbols.
struct LambdaCollector {
    std::vector<const ast::LambdaExpr*>& out;
    // §27.5 / #672: when true, also descend INTO each recorded lambda's body so
    // every nested LambdaExpr is collected (one entry per closure env). When
    // false (the #754 default), only the outermost lambdas are recorded.
    bool descend = false;

    void expr(const ast::Expr* e) {
        if (!e) return;
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
            out.push_back(lam);
            if (descend) {
                if (lam->exprBody) expr(lam->exprBody.get());
                for (auto& s : lam->stmts) stmt(s.get());
            }
            return;  // outermost only unless descend is set
        }
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(m->target.get()); return; }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get()); return; }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            return; }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            return; }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get());
            for (auto& a : ix->indices) expr(a.value.get());
            return; }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get()); expr(b->right.get()); return; }
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get()); return; }
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get()); return; }
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get()); return; }
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get()); expr(c->thenValue.get());
            expr(c->elseValue.get()); return; }
        if (auto* t = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(t->operand.get()); return; }
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(r->lo.get()); expr(r->hi.get()); expr(r->step.get());
            return; }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get());
            return; }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get());
            for (auto& a : w->assigns) expr(a.value.get());
            return; }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get());
            for (auto& k : cl->keys) expr(k.get());
            return; }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : is->parts) expr(part.expr.get());
            return; }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get());
            for (auto& arm : ce->arms) {
                expr(arm.guard.get());
                expr(arm.exprBody.get());
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return; }
        // Literals and other leaf expressions: nothing to do.
    }

    void stmt(const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            expr(ld->initializer.get()); return; }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get()); return; }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            expr(r->value.get()); return; }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            expr(es->expr.get()); return; }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get()); return; }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            expr(as->target.get()); expr(as->value.get()); return; }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            expr(inc->target.get()); return; }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            expr(ds->value.get()); return; }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get()); return; }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get());
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get());
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return; }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get());
            for (auto& b : ws->body) stmt(b.get());
            return; }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            expr(fs->iterable.get()); expr(fs->withExpr.get());
            for (auto& b : fs->body) stmt(b.get());
            return; }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return; }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) stmt(b.get());
            for (auto& c : ts->catches) {
                expr(c.guard.get());
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return; }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get());
            for (auto& arm : cs->arms) {
                expr(arm.guard.get());
                expr(arm.exprBody.get());
                for (auto& a : arm.stmts) stmt(a.get());
            }
            return; }
        // BreakStmt / ContinueStmt: no sub-expressions.
    }
};
}  // namespace


void Emitter::collectCapturedSyms(
        const ast::LambdaExpr& lam,
        std::unordered_set<const sema::Symbol*>& out) {
    CaptureWalker w{ctx_, out};
    if (lam.exprBody) w.expr(lam.exprBody.get());
    for (auto& s : lam.stmts) w.stmt(s.get());
}


// §27.5 / #672: collect EVERY LambdaExpr in the body (full descent into nested
// lambda bodies). Each distinct node is one closure env.
void Emitter::collectAllLambdas(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        std::vector<const ast::LambdaExpr*>& out) {
    LambdaCollector lc{out, /*descend=*/true};
    for (auto& s : body) lc.stmt(s.get());
}


// §27.5 / #754: does any closure in `fs.body` capture the for-loop variable?
bool Emitter::forVarCapturedInBody(const ast::ForStmt& fs) {
    std::vector<const ast::LambdaExpr*> lambdas;
    LambdaCollector lc{lambdas};
    for (auto& s : fs.body) lc.stmt(s.get());
    for (const ast::LambdaExpr* lam : lambdas) {
        std::unordered_set<const sema::Symbol*> caps;
        collectCapturedSyms(*lam, caps);
        for (const sema::Symbol* s : caps) {
            if (auto* loc = dynamic_cast<const sema::LocalSymbol*>(s)) {
                if (loc->decl == &fs) return true;
            }
        }
    }
    return false;
}


llvm::Value* Emitter::makeMethodThunk(sema::MethodSymbol* ms,
                             llvm::Value* receiverVal,
                             const sema::TypeRef* recvStaticTy) {
    llvm::Function* origFn = nullptr;
    auto it = methodFns_.find(ms);
    if (it != methodFns_.end()) origFn = it->second;
    // §27.6 (#738): a method reference to a VIRTUAL or INTERFACE method dispatches
    // DYNAMICALLY through the receiver's vtable/itable when invoked (not the
    // statically-resolved origFn). An INTERFACE method has no concrete body /
    // methodFns_ entry, so derive its LLVM signature from methodFnType.
    bool instMethod = !isStaticMethod(ms);
    // §27.6 (#1306): an UNBOUND extension-method reference (`Vec.Scaled`) has
    // the receiver as the function value's FIRST PARAMETER (supplied by the
    // caller at the call site), never captured in the env. A BOUND extension-
    // method reference (`x.Double`) captures the instance like any instance-
    // method ref (#457). The two are distinguished by whether the caller
    // lowered a receiver: a type-name qualifier yields a null receiverVal
    // (lowerExpr on a bare type name returns null), an instance qualifier
    // yields the value. So extRef is true ONLY for the unbound case.
    bool extRef = isExtensionMethod(ms) && !receiverVal;
    if (extRef) instMethod = false;
    auto ifaceIt = ifaceMethodIndex_.find(ms);
    auto vtSlotIt = vtableSlot_.find(ms);
    bool dynamicDispatch = instMethod &&
        (ifaceIt != ifaceMethodIndex_.end() || vtSlotIt != vtableSlot_.end());
    llvm::FunctionType* methTy =
        origFn ? origFn->getFunctionType() : methodFnType(ms);
    if (!methTy) return nullptr;
    // fix #2766 / fix #2780 / §27.6 / §14.1: an INTERFACE-dispatch thunk (no
    // concrete origFn) must use the INSTANTIATED parameter types AND RETURN
    // TYPE of the receiver's static interface type, not methodFnType(ms)'s
    // erased ones. methodFnType lowers the interface's DECLARED parameter
    // types; for a generic interface (`IEquatable<T>.Equals(other: T)`,
    // `IBox<T>.Get(): T`) the unbound type-parameter reference lowers to a
    // placeholder `i32` for EVERY instantiation. That is the ABI the emitted
    // thunk then advertises, while both the call site (which types the call
    // from the reference's static `(string) -> bool` / `(string) -> string`)
    // and the itable target use the real one.
    //
    // The consequence is not cosmetic — it was measured on the generated code:
    // for #2766 the thunk compiled to `mov %esi,%ebx` / `mov %ebx,%esi`, i.e.
    // it TRUNCATED the reference argument to 32 bits before forwarding it. A
    // string argument whose address happens to be below 4 GiB (a literal in a
    // small, non-PIE executable) survives the round trip, which is why this
    // went unnoticed; a HEAP string does not. For #2780 the same defect at the
    // RETURN position (a user-defined `IBox<string>.Get(): string` thunk
    // advertised `i32 (...)` while the callee returned `ptr`) manifested as a
    // plain-oracle "Both operands to ICmp instruction are not of the same
    // type" IR-verification failure — the caller compared the i32 result to a
    // string pointer.
    //
    // Generalised for #2780: rather than the prior hardcoded special case
    // (scoped to the two stdlib-stated single-parameter signatures
    // IEquatable<T>.Equals / IComparable<T>.CompareTo), the substitution keys
    // on EACH parameter AND the RETURN type whose DECLARED type is a bare
    // type-parameter reference matching `ms->parent->genericParams[i]->name`
    // — substituted with `recvStaticTy->genericArgs[i]`. Skips by-ref
    // parameters (§22.3) — methodFnType already passes those as a pointer-to-
    // slot, which is correct. A non-generic interface (`IFoo.Do(n: int): int`)
    // is bit-for-bit unaffected: every parameter and the return type has a
    // concrete declared type, so the predicate does not match.
    //
    // Detecting a generic-parameter reference uses EITHER the parameter
    // symbol's `resolvedDeclType` (set on imported methods by ModuleLoader)
    // OR the AST node's `declType->name->parts[0]` (the synthetic builtin
    // interface methods carry no `resolvedDeclType` — which is precisely why
    // `methodFnType` fell back to the `i32` placeholder — but their
    // `ps->declType` AST node carries the bare name `T`). Both feed
    // `isBareTypeParamRef`, which is the same predicate every other generic-
    // substitution site uses, so the substitution shape is uniform.
    if (!origFn && dynamicDispatch && instMethod && !extRef && recvStaticTy &&
        ms->parent && recvStaticTy->kind == sema::TypeRef::Kind::Named &&
        recvStaticTy->declaration == ms->parent &&
        recvStaticTy->genericArgs.size() == ms->parent->genericParams.size()) {
        // Map type-param name -> generic-args index. Linear scan over a small
        // list (stdlib interfaces: 1-2 entries; no realistic size limit for
        // user interfaces, but the position-by-name lookup is still cheap).
        auto findGenericArgIdx = [&](std::string_view pname) -> int {
            for (std::size_t i = 0; i < ms->parent->genericParams.size(); ++i) {
                if (ms->parent->genericParams[i] &&
                    ms->parent->genericParams[i]->name == pname)
                    return static_cast<int>(i);
            }
            return -1;
        };
        // Resolve a parameter position to the GENERIC-PARAMETER NAME its
        // declared type refers to ("" when the declared type is concrete).
        // Two evidence sources: the symbol-side `resolvedDeclType` (imported
        // methods), and the AST-side `declType->name` (synthetic builtin
        // interface methods whose resolvedDeclType is null by design).
        auto paramGenericParamName = [&](std::size_t i) -> std::string_view {
            if (i >= ms->params.size()) return {};
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(ms->params[i]);
            if (!ps) return {};
            if (ps->resolvedDeclType &&
                isBareTypeParamRef(ps->resolvedDeclType))
                return ps->resolvedDeclType->headName();
            const ast::Type* dt = ps->declType;
            if (dt && dt->name && dt->name->parts.size() == 1 &&
                !isBuiltinTypeRefName(dt->name->parts[0]))
                return dt->name->parts[0];
            return {};
        };
        bool changed = false;
        std::vector<llvm::Type*> newParamTys(methTy->param_begin(),
                                             methTy->param_end());
        for (std::size_t i = 0; i < ms->params.size(); ++i) {
            // Skip by-ref parameters (§22.3): methodFnType lowers them as
            // pointer-to-slot, which is already correct for the thunk's ABI.
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(ms->params[i]);
            if (ps && isByRefMod(ps->modifier)) continue;
            std::string_view pname = paramGenericParamName(i);
            if (pname.empty()) continue;
            int gi = findGenericArgIdx(pname);
            if (gi < 0 ||
                gi >= static_cast<int>(recvStaticTy->genericArgs.size()))
                continue;
            sema::TypeRef* arg = recvStaticTy->genericArgs[gi];
            if (!arg) continue;
            llvm::Type* nt = lowerTypeRef(arg);
            if (!nt || nt == newParamTys[i + 1]) continue;
            newParamTys[i + 1] = nt;  // +1: param 0 is the `this` ptr
            changed = true;
        }
        // Return type — substitute when the declared return is a bare
        // type-parameter reference (e.g. `IBox<T>.Get(): T` with T = string).
        llvm::Type* newRetTy = methTy->getReturnType();
        // SEAM-ACCESSOR: MethodSymbol has no resolved-return-type field
        // (Symbol.hpp: genericParams/params/isOperator/operatorToken/
        // extensionDecl — no returnType), so `ms->decl->returnType` is the
        // ONLY source of the declared return; #2780's generalised thunk
        // substitution MUST read the AST MethodDecl here.
        if (const ast::MethodDecl* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
            if (md->returnType) {
                const sema::TypeRef* retRt = ctx_.resolvedType(*md->returnType);
                if (isBareTypeParamRef(retRt)) {
                    int gi = findGenericArgIdx(retRt->headName());
                    if (gi >= 0 &&
                        gi < static_cast<int>(recvStaticTy->genericArgs.size())) {
                        if (sema::TypeRef* arg = recvStaticTy->genericArgs[gi]) {
                            if (llvm::Type* nt = lowerTypeRef(arg)) {
                                if (nt != newRetTy) newRetTy = nt;
                            }
                        }
                    }
                }
            }
        }
        if (newRetTy != methTy->getReturnType()) changed = true;
        if (changed) {
            methTy = llvm::FunctionType::get(newRetTy, newParamTys,
                                             /*isVarArg=*/false);
        }
    }
    // A non-virtual, non-interface method ref must have a concrete origFn.
    if (!origFn && !dynamicDispatch) return nullptr;

    // §27.7.1 (#1544): classify the function-value equality kind from the
    // method symbol + whether a receiver is captured. Computed once here and
    // threaded into buildMethodClosureCell on both the cache-hit and cache-miss
    // paths so the kind tag is consistent across every reference to this method.
    FnValKind kind = closureKindForMethod(ms, extRef);

    // #1170 / §24.4: reuse a cached thunk for this method so a named method
    // reference resolves to a STABLE fn pointer (the cell's fn at offset 0).
    // This lets `event -= OnTick` match the `event += OnTick` handler by value
    // (fn + receiver) — see bz_event_remove_closure. The thunk body is
    // receiver-independent (it loads `this` from env), so one thunk per method
    // is correct. On a cache hit skip re-emitting the body and go straight to
    // building a fresh closure cell with the current receiver.
    // fix #2766: the LLVM signature is part of the key — see methodThunks_'s
    // declaration. Same method + same instantiated signature still yields ONE
    // thunk, so #1170's `+=`/`-=` identity is unaffected.
    if (auto cit = methodThunks_.find({ms, extRef, methTy});
        cit != methodThunks_.end())
        return buildMethodClosureCell(cit->second, instMethod, receiverVal, kind);

    // Build the thunk's parameter types: (ptr env, user_params...).
    std::vector<llvm::Type*> thunkParamTys;
    thunkParamTys.push_back(ptrTy());  // env_ptr
    if (extRef) {
        // §27.6 (#1306): the receiver is the thunk's first USER parameter.
        // Use the receiver's VALUE representation (lowerType of the extension's
        // target type) so it matches what the caller passes — for a record that
        // is the first-class struct value, for a class the object pointer, for a
        // primitive/enum the value. origFn expects a (possibly differently)
        // shaped `this` (record → spilled pointer); that shaping happens in the
        // body below. Then append origFn's explicit params (skip its `this`
        // arg 0), which already match what the caller passes.
        if (const ast::Type* tt = ms->extensionDecl
                ? ms->extensionDecl->targetType.get() : nullptr)
            thunkParamTys.push_back(lowerType(tt));
        else
            thunkParamTys.push_back(ptrTy());
        for (std::size_t i = 1; i < methTy->getNumParams(); ++i)
            thunkParamTys.push_back(methTy->getParamType(i));
    } else {
        // Skip the leading `this` arg for instance methods
        // (it comes from env instead).
        for (std::size_t i = (instMethod ? 1 : 0);
             i < methTy->getNumParams(); ++i)
            thunkParamTys.push_back(methTy->getParamType(i));
    }
    llvm::Type* retTy = methTy->getReturnType();
    auto* thunkFnTy = llvm::FunctionType::get(retTy, thunkParamTys, false);

    // Create thunk function.
    static std::atomic<unsigned> thunkCtr{0};
    std::string thunkName = "thunk$" + std::to_string(thunkCtr.fetch_add(1));
    auto* thunk = llvm::Function::Create(
        thunkFnTy, llvm::Function::InternalLinkage, thunkName, module_.get());

    // Save emitter state.
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    llvm::BasicBlock* savedBB = builder_.GetInsertBlock();
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    // #2281 / §31.2: the method thunk is a separate function. Preserve the
    // caller's active try/finally frames across this nested emission; keep the
    // parallel finally-body stack depth-aligned per §29.7.
    auto savedFinallyStack = finallyStack_;
    auto savedFinallyBodies = finallyBodyStack_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::BasicBlock* savedProp = propagateBB_;
    llvm::Value* savedCaught = currentCaughtExc_;
    std::size_t savedUnwindDepth = unwindScopeDepth_;
    bool savedDiverged = diverged_;

    currentFn_ = thunk;
    currentThis_ = nullptr;
    locals_.clear(); paramSlots_.clear(); refParams_.clear();
    disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
    finallyBodyStack_.clear();
    unwindTarget_ = nullptr; propagateBB_ = nullptr;
    currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0; diverged_ = false;
    builder_.SetInsertPoint(
        llvm::BasicBlock::Create(llvm_, "entry", thunk));

    // Build call args to the original function.
    std::vector<llvm::Value*> callArgs;
    auto argIt = thunk->args().begin();
    llvm::Value* envPtrArg = &*argIt++;  // env_ptr (arg 0)
    if (extRef) {
        // §27.6 (#1306): the receiver is the thunk's first user param. Shape it
        // to origFn's `this` ABI: a RECORD receiver is passed to origFn as a
        // pointer to a spilled copy (so the body's `this.field` reuses record
        // field access — §26.1 / #433); the thunk receives the value, so spill
        // it. Class receivers are already ptr; primitive/enum receivers are by
        // value and need no shaping. origFn's `this` type is methTy param 0.
        llvm::Value* recvArg = &*argIt++;
        llvm::Type* origThisTy = methTy->getParamType(0);
        if (origThisTy->isPointerTy() && !recvArg->getType()->isPointerTy()) {
            llvm::AllocaInst* tmp = entryAlloca(recvArg->getType());
            builder_.CreateStore(recvArg, tmp);
            callArgs.push_back(tmp);
        } else {
            callArgs.push_back(recvArg);
        }
    } else if (instMethod) {
        // §26.1 / #457 / #1636: Load receiver from env with the CORRECT `this`
        // representation. For a CLASS/INTERFACE extension (or a regular
        // instance method) `this` is a ptr and the env block already holds
        // that same ptr directly (buildMethodClosureCell stores receiverVal
        // verbatim) — load ptr. For a PRIMITIVE or ENUM extension method
        // `this` is passed BY VALUE (e.g. i32 for `int`); the env block holds
        // the raw value bytes and we must load that type. For a RECORD
        // extension method `this` is a POINTER TO A SPILLED COPY (§26.1 /
        // #433 — so the body's `this.field` reuses record field access), but
        // the env block holds the record's VALUE itself (receiverVal is the
        // struct value the lowered receiver expression produced, sized by
        // buildMethodClosureCell via getTypeStoreSize) — never a pointer.
        // Loading `ptr` there (the pre-#1636 code) reads the struct's leading
        // bytes as a garbage pointer and SIGSEGVs when origFn dereferences it.
        // Load the record's actual struct type from env, then spill it to a
        // fresh alloca and pass that address — mirroring the record-receiver
        // shaping applied at direct (non-thunk) extension-method call sites
        // (LlvmBackend_Calls2.cpp's `isExtensionMethod(ms)` receiver-shaping
        // block).
        llvm::Type* thisLoadTy = ptrTy();  // default: pointer (class/record)
        bool recordEnv = false;
        if (isExtensionMethod(ms)) {
            // The actual `this` type is origFn->getArg(0)->getType().
            llvm::Type* extThisTy = origFn->getArg(0)->getType();
            if (!extThisTy->isPointerTy()) {
                thisLoadTy = extThisTy;
            } else if (const ast::Type* tt =
                           ms->extensionDecl ? ms->extensionDecl->targetType.get()
                                              : nullptr) {
                const sema::TypeRef* rt = ctx_.resolvedType(*tt);
                if (rt && rt->kind == sema::TypeRef::Kind::Named &&
                    rt->declaration &&
                    rt->declaration->kind == ast::TypeDeclKind::Record) {
                    thisLoadTy = recordStruct(rt->declaration);
                    recordEnv = true;
                }
            }
        }
        llvm::Value* loadedThis = builder_.CreateLoad(thisLoadTy, envPtrArg);
        if (recordEnv) {
            llvm::AllocaInst* tmp = entryAlloca(thisLoadTy);
            builder_.CreateStore(loadedThis, tmp);
            callArgs.push_back(tmp);
        } else {
            callArgs.push_back(loadedThis);
        }
    }
    for (; argIt != thunk->args().end(); ++argIt)
        callArgs.push_back(&*argIt);

    // §27.6 (#738): dispatch through the receiver's vtable/itable for a virtual
    // or interface method (mirrors emitMethodCall's dynamic path); otherwise call
    // the concrete origFn directly. callArgs[0] is the receiver.
    llvm::Value* dynFnPtr = nullptr;
    // fix #2766 (§4.6 / stdlib §9.6): state for the string-receiver arm below.
    bool strRecvPossible = false;
    llvm::BasicBlock* dispEndBB = nullptr;
    llvm::BasicBlock* strDispEnd = nullptr;
    llvm::BasicBlock* strNeBB = nullptr;
    llvm::Value* strDispVal = nullptr;
    if (dynamicDispatch && !callArgs.empty() &&
        callArgs[0]->getType()->isPointerTy()) {
        llvm::Value* recv = callArgs[0];
        // fix #2766 / §4.6 / stdlib §9.6: the SAME defect #2752 fixed in
        // emitMethodCall's interface arm, at the METHOD-GROUP site. stdlib §9.6
        // declares
        //   `public class String: IEnumerable<char>, IComparable<string>,
        //    IEquatable<string>`
        // so `var eq: IEquatable<string> := s;` is a legal conversion — but a
        // §4.6 string is a runtime-managed REFERENCE, and the slot holds a bare
        // `bz_string*` whose word 0 is `char* data`, NOT a vtable (see
        // bezel_runtime.h's bz_obj_is_string contract, §8.6/§12.7 #417). The
        // itmap load at vptr[1] below therefore reads `data + 8` — 8 bytes past
        // the payload — and SIGSEGVs (exit 139 on the plain oracle; ASan reports
        // a global-/heap-buffer-overflow "in thunk$0").
        //
        // ⚠ THE FIX BELONGS HERE, IN THE THUNK BODY, NOT AT THE MINT SITE.
        // Thunks are cached in methodThunks_ keyed {ms, extRef} and the body is
        // receiver-independent, so a decision taken where the thunk is minted
        // would be wrong for every other receiver reusing the cached thunk.
        // Measured, not assumed: a program forming `IEquatable<string>` method
        // groups over a user class AND over a string AND an `IEquatable<Object>`
        // group emits exactly ONE `thunk$0`; likewise one thunk serves both
        // `IComparable<int>` (boxed primitive) and `IComparable<string>`. The
        // discrimination must therefore be a RUN-TIME test, which is what this is.
        //
        // ⚠ REACHABILITY ARGUMENT, RE-ESTABLISHED FOR THIS SITE (#2766 requires
        // this rather than copying #2752's diamond on faith). bz_obj_is_string is
        // a whitelist NEGATION (bezel_runtime_refcounting.cpp): it answers
        // "string" for ANY pointer whose word 0 is absent from bz$vtable$table,
        // and that table omits several vtable kinds — bz_array_t (no vtable word
        // at all), bz_lazy_vtable (every lazy LINQ pipeline), the iterator /
        // async-frame / collection-adapter vtables. So the question is NOT "does
        // the receiver's type declare the interface?" (`Object` admits anything,
        // so that question has no force) but: can a value with an unregistered
        // word 0 reach THIS site, and did it previously dispatch CORRECTLY here?
        //   * It CAN reach here, only through the IEquatable<Object> /
        //     IComparable<Object> acceptance (#2765).
        //   * It does NOT dispatch correctly today: both shapes were probed on
        //     this exact method-group path and both already crash INSIDE thunk$0
        //     before this change — `Array<int>` through `IEquatable<Object>` is a
        //     heap-buffer-overflow, and a `.Where(...)` lazy pipeline is a null
        //     deref (bz_lazy_vtable's word 1 is not an itmap).
        // So no previously correct program becomes incorrect. The shapes that DO
        // dispatch correctly today — a user/imported class declaring the
        // interface, and a boxed primitive/value-builtin — all have REGISTERED
        // vtables, so bz_obj_is_string answers "not a string" for them and they
        // keep taking the itable path unchanged (probed: both still return their
        // pre-change values).
        //
        // The static gate below only decides whether to EMIT the tests; it is
        // deliberately as narrow as #2752's (the interface arm of exactly
        // IEquatable.Equals / IComparable.CompareTo). It never fires on the
        // VTABLE arm and never on a wider interface such as IEnumerable, where an
        // unregistered-vtable receiver genuinely does dispatch correctly today
        // and a false positive would be a real regression.
        strRecvPossible =
            ifaceIt != ifaceMethodIndex_.end() && ms->parent &&
            callArgs.size() == 2 && callArgs[1] &&
            callArgs[1]->getType()->isPointerTy() && !retTy->isVoidTy() &&
            ((ms->parent->name == "IEquatable"  && ms->name == "Equals") ||
             (ms->parent->name == "IComparable" && ms->name == "CompareTo")) &&
            descArray_.count(ms->parent) != 0;
        if (strRecvPossible) {
            bool isEqualsMethod = ms->name == "Equals";
            auto* strDispBB =
                llvm::BasicBlock::Create(llvm_, "thunk.str", thunk);
            auto* objDispBB =
                llvm::BasicBlock::Create(llvm_, "thunk.obj", thunk);
            auto* mixedBB =
                llvm::BasicBlock::Create(llvm_, "thunk.mixed", thunk);
            dispEndBB = llvm::BasicBlock::Create(llvm_, "thunk.dend", thunk);
            strNeBB = llvm::BasicBlock::Create(
                llvm_, isEqualsMethod ? "thunk.strne" : "thunk.strtrap", thunk);
            auto* isStrFn =
                declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
            // BOTH operands are tested, not just the receiver — the lesson
            // #2752 needed two rounds to learn. emitStringEq / emitStringOrdCmp
            // dereference `l` AND `r` as bz_string (loadStringLen reads offset
            // 8, loadStringData offset 0), so a receiver-only guard would read a
            // non-string ARGUMENT's vtable pointer as `char* data` and answer
            // from whatever bytes follow it — a silent wrong answer, no crash.
            // That argument is reachable here: one cached thunk serves
            // IEquatable<Object> too, where the argument is statically `Object`
            // and need not be a string. For IEquatable<string> /
            // IComparable<string> the argument is statically `string`, so the
            // extra test always passes and behaviour is unchanged.
            llvm::Value* rIsStr = builder_.CreateICmpNE(
                builder_.CreateCall(isStrFn, {recv, vtableTablePtr()},
                                    "thunk.risstr"),
                builder_.getInt32(0));
            llvm::Value* aIsStr = builder_.CreateICmpNE(
                builder_.CreateCall(isStrFn, {callArgs[1], vtableTablePtr()},
                                    "thunk.aisstr"),
                builder_.getInt32(0));
            builder_.CreateCondBr(
                builder_.CreateAnd(rIsStr, aIsStr, "thunk.bothstr"),
                strDispBB, mixedBB);
            builder_.SetInsertPoint(mixedBB);
            builder_.CreateCondBr(rIsStr, strNeBB, objDispBB);
            builder_.SetInsertPoint(strNeBB);
            if (isEqualsMethod) {
                // §9.6 answers a string-vs-non-string comparison directly:
                //   public pure override method Equals(other: Object?): bool;
                //     // content-based, not reference-based
                // so the answer is FALSE. Neither operand is dereferenced, and
                // the PHI below supplies the `false`.
            } else {
                // CompareTo: §9.6 declares only
                //   public pure method CompareTo(other: string): int  // ordinal
                // and spec §4.6 confines ordinal comparison to string operands,
                // so there is NO ordering to return and inventing one would be a
                // silent fabrication. Falling through to the itable path is not
                // a "loud failure" either — bz_itable_lookup would scan the
                // string payload as an itmap and then call through itab[k], an
                // indirect call through arbitrary memory. Trap instead: one
                // block, invents no ordering, strictly defined. Reachable only
                // through the IComparable<Object> acceptance (#2765); for
                // IComparable<string> the argument is statically a string, so
                // this arm is dead there.
                builder_.CreateCall(
                    declareC("abort", llvm::Type::getVoidTy(llvm_), {}), {});
            }
            builder_.CreateBr(dispEndBB);
            // Both operands are strings: §9.6 content equality / ordinal
            // comparison. emitStringEq is null-safe on both operands;
            // emitStringOrdCmp requires non-null, which §4.6 guarantees for an
            // ordering operand ("Ordering requires non-nullable operands").
            builder_.SetInsertPoint(strDispBB);
            if (isEqualsMethod) {
                strDispVal = emitStringEq(recv, callArgs[1]);
            } else if (llvm::Value* c = emitStringOrdCmp(recv, callArgs[1])) {
                strDispVal = c->getType()->isIntegerTy(32)
                    ? c
                    : builder_.CreateTrunc(c, i32(), "thunk.str.cmp");
            }
            // Match the dispatched slot's return type so the merge PHI is
            // well-typed (Equals → i1, CompareTo → i32).
            if (strDispVal && strDispVal->getType() != retTy &&
                strDispVal->getType()->isIntegerTy() && retTy->isIntegerTy())
                strDispVal = builder_.CreateZExtOrTrunc(strDispVal, retTy,
                                                        "thunk.str.ret");
            // The intrinsics can emit their own blocks, so the PHI's incoming
            // block is wherever the builder ended up, not strDispBB.
            strDispEnd = builder_.GetInsertBlock();
            builder_.CreateBr(dispEndBB);
            builder_.SetInsertPoint(objDispBB);
        }
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        if (ifaceIt != ifaceMethodIndex_.end()) {
            // §13 itable: vptr[1] = itmap; bz_itable_lookup(itmap, td$I)[k].
            auto descIt = descArray_.find(ms->parent);
            if (descIt != descArray_.end()) {
                llvm::Value* itmapPtr = builder_.CreateLoad(
                    ptrTy(),
                    builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(1)}));
                auto* lookupFn = declareC("bz_itable_lookup", ptrTy(),
                                          {ptrTy(), ptrTy()});
                llvm::Value* itab = builder_.CreateCall(
                    lookupFn, {itmapPtr, descIt->second}, "thunk.itab");
                dynFnPtr = builder_.CreateLoad(
                    ptrTy(),
                    builder_.CreateGEP(
                        ptrTy(), itab,
                        {builder_.getInt32(ifaceIt->second)}),
                    "thunk.ifn");
            }
        } else {
            // §8.6/§9.6 vtable: virtuals base at slot 6 (kVirtualMethodsBase;
            // [3]=ToString [4]=Equals [5]=GetHashCode). vtableSlot_ is absolute.
            dynFnPtr = builder_.CreateLoad(
                ptrTy(),
                builder_.CreateGEP(ptrTy(), vptr,
                                   {builder_.getInt32(vtSlotIt->second)}),
                "thunk.vfn");
        }
    }
    llvm::Value* result =
        dynFnPtr ? builder_.CreateCall(methTy, dynFnPtr, callArgs)
                 : builder_.CreateCall(origFn, callArgs);
    // fix #2766: merge the string-receiver arm back in. Only reached when the
    // diamond above was emitted.
    if (strRecvPossible && strDispVal && strDispEnd && strNeBB && dispEndBB &&
        strDispVal->getType() == result->getType()) {
        llvm::BasicBlock* objDispEnd = builder_.GetInsertBlock();
        builder_.CreateBr(dispEndBB);
        builder_.SetInsertPoint(dispEndBB);
        auto* phi = builder_.CreatePHI(result->getType(), 3, "thunk.res");
        phi->addIncoming(strDispVal, strDispEnd);
        // §9.6 mixed-operand arm. For Equals this IS the answer — string vs
        // non-string is false, and getNullValue is `false` for the i1 return
        // type. For CompareTo the arm aborts before reaching here, so the
        // incoming value is unreachable and exists only to keep the PHI
        // well-formed.
        phi->addIncoming(llvm::Constant::getNullValue(result->getType()),
                         strNeBB);
        phi->addIncoming(result, objDispEnd);
        result = phi;
    } else if (strRecvPossible && dispEndBB) {
        // Defensive: an intrinsic returned null, or the arms disagreed on type
        // (neither should happen for the two gated signatures). Keep the CFG
        // well-formed by joining every arm and taking the dispatched result —
        // never leave a block without a terminator.
        llvm::BasicBlock* objDispEnd = builder_.GetInsertBlock();
        builder_.CreateBr(dispEndBB);
        if (strDispEnd && !strDispEnd->getTerminator()) {
            builder_.SetInsertPoint(strDispEnd);
            builder_.CreateBr(dispEndBB);
        }
        builder_.SetInsertPoint(dispEndBB);
        auto* phi = builder_.CreatePHI(result->getType(), 3, "thunk.res");
        if (strDispEnd)
            phi->addIncoming(llvm::UndefValue::get(result->getType()),
                             strDispEnd);
        if (strNeBB)
            phi->addIncoming(llvm::Constant::getNullValue(result->getType()),
                             strNeBB);
        phi->addIncoming(result, objDispEnd);
        result = phi;
    }
    if (retTy->isVoidTy()) builder_.CreateRetVoid();
    else builder_.CreateRet(result);

    // Restore emitter state.
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals); paramSlots_ = std::move(savedParams);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);          // #2281 / §31.2
    finallyBodyStack_ = std::move(savedFinallyBodies);     // #2281 / §29.7
    unwindTarget_ = savedUnwind; propagateBB_ = savedProp;
    currentCaughtExc_ = savedCaught; unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    if (savedBB) builder_.SetInsertPoint(savedBB);

    // #1170 / §24.4: cache the thunk so subsequent references to this method
    // (e.g. the `-=` matching an earlier `+=`) reuse the SAME fn pointer.
    methodThunks_[{ms, extRef, methTy}] = thunk;

    return buildMethodClosureCell(thunk, instMethod, receiverVal, kind);
}


// fix #2781 / §8.1 / §14.2: a method-group reference on an Object-typed
// receiver to one of Object's three universal instance members —
// `o.ToString`, `o.Equals`, `o.GetHashCode` — has no MethodSymbol bound
// (Object is a primitive; its virtuals are runtime intrinsics, the receiver's
// static interface admits anything, and sema deliberately leaves the
// MemberAccessExpr symbol null rather than synthesising one). The prior
// CG60 path returned nullptr for this shape, so the method-group reference
// degraded to the §27.5 #2434 zero-init: `var f := o.GetHashCode;` made
// `f` a null closure whose `f()` returned 0, and `var f := o.ToString;`
// inside an interpolation ICEd on a mistyped ICmp. Fix: mint a thunk
// closure here that mirrors lowerObjectToString / lowerObjectEquals /
// lowerObjectGetHashCode's diamond — null receiver → §8.3/§8.4/§8.5
// default; a bz_string* receiver (no vtable at word 0, §4.6) → the string
// intrinsic (+ retain for ToString per #2846, since lowerObjectToString's
// contract is +1); a class/box receiver → vtable[fixed slot] dispatch to
// the user override (if any) or the per-class default thunk minted by
// classToStringThunk / classEqualsThunk / classHashCodeThunk. The thunk's
// signature is `T(ptr env [, ptr other])` matching the §27.6 closure ABI;
// the env stores the BORROW of the receiver (cell does not own it, §28.3).
// `ma` is the MemberAccessExpr; the function does its own lowerExpr on
// `ma->target` to obtain the receiver value at the use site, then stores
// it in a fresh env block.
llvm::Value* Emitter::makeObjectMemberThunk(const ast::MemberAccessExpr& ma,
                                             std::string_view memberName) {
    if (!ma.target) return nullptr;
    // Identify the slot from the member name. The arity check is implicit
    // in the name (ToString/GetHashCode are nullary, Equals takes one arg)
    // and the only call site that lands here is the Object-receiver arm
    // of the CG60 path in LlvmBackend_Exprs.cpp — already gated on
    // memberName being one of the three and arity matching.
    unsigned fixedSlot = 0;
    llvm::Type* retTy = nullptr;
    bool takesOther = false;
    if (memberName == "ToString") {
        fixedSlot = kToStringSlot;
        retTy = ptrTy();     // §4.6 string pointer
    } else if (memberName == "Equals") {
        fixedSlot = kEqualsSlot;
        retTy = builder_.getInt1Ty();
        takesOther = true;
    } else if (memberName == "GetHashCode") {
        fixedSlot = kGetHashCodeSlot;
        retTy = i32();
    } else {
        return nullptr;
    }
    // Lower the receiver value at the use site (a fresh lowerExpr, just
    // like makeMethodThunk's caller does). The thunk captures the result
    // in its env so the call site does NOT need to pass it.
    llvm::Value* recvVal = lowerExpr(*ma.target);
    if (!recvVal || !recvVal->getType()->isPointerTy()) return nullptr;
    // Build the thunk's signature: retTy (ptr env [, ptr other]).
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());  // env
    if (takesOther) paramTys.push_back(ptrTy());  // other
    auto* thunkFnTy = llvm::FunctionType::get(retTy, paramTys, false);
    static std::atomic<unsigned> objThunkCtr{0};
    std::string thunkName = "objthunk$" +
        std::string(memberName) + "$" +
        std::to_string(objThunkCtr.fetch_add(1));
    auto* thunk = llvm::Function::Create(
        thunkFnTy, llvm::Function::InternalLinkage, thunkName, module_.get());
    // Save emitter state (mirrors makeMethodThunk's preamble).
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    llvm::BasicBlock* savedBB = builder_.GetInsertBlock();
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    auto savedFinallyStack = finallyStack_;
    auto savedFinallyBodies = finallyBodyStack_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::BasicBlock* savedProp = propagateBB_;
    llvm::Value* savedCaught = currentCaughtExc_;
    std::size_t savedUnwindDepth = unwindScopeDepth_;
    bool savedDiverged = diverged_;
    currentFn_ = thunk;
    currentThis_ = nullptr;
    locals_.clear(); paramSlots_.clear(); refParams_.clear();
    disposeStack_.clear(); loopStack_.clear();
    finallyStack_.clear(); finallyBodyStack_.clear();
    unwindTarget_ = nullptr; propagateBB_ = nullptr;
    currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0; diverged_ = false;
    builder_.SetInsertPoint(
        llvm::BasicBlock::Create(llvm_, "entry", thunk));
    // Arg 0 is env — points to a heap block holding the receiver
    // (captured via buildMethodClosureCell below). Load it inside the
    // thunk since SSA values from the outer function are not in scope
    // here.
    auto argIt = thunk->args().begin();
    llvm::Value* envPtr = &*argIt++;
    llvm::Value* recvInThunk = builder_.CreateLoad(ptrTy(), envPtr,
                                                  "objthunk.recv");
    llvm::Value* otherArg = takesOther ? &*argIt++ : nullptr;
    // Build the diamond. Each arm produces a value of retTy. The three
    // arms must produce the same type for the PHI to be well-formed.
    auto* nullBB = llvm::BasicBlock::Create(
        llvm_, "objthunk.null", thunk);
    auto* liveBB = llvm::BasicBlock::Create(
        llvm_, "objthunk.live", thunk);
    auto* endBB = llvm::BasicBlock::Create(
        llvm_, "objthunk.end", thunk);
    llvm::Value* isNull = builder_.CreateICmpEQ(
        recvInThunk, llvm::ConstantPointerNull::get(ptrTy()),
        "objthunk.isnull");
    builder_.CreateCondBr(isNull, nullBB, liveBB);
    // null receiver → §8.3/§8.4/§8.5 default.
    builder_.SetInsertPoint(nullBB);
    llvm::Value* nullRes = nullptr;
    if (fixedSlot == kToStringSlot) {
        nullRes = lowerStringLiteral("\"null\"", false);
    } else if (fixedSlot == kEqualsSlot) {
        nullRes = builder_.CreateICmpEQ(
            recvInThunk, otherArg ? otherArg : recvInThunk,
            "objthunk.nulleq");
    } else {
        nullRes = builder_.getInt32(0);
    }
    llvm::BasicBlock* nullEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);
    // live: discriminate string vs vtable-bearing.
    builder_.SetInsertPoint(liveBB);
    auto* isStrFn = declareC("bz_obj_is_string",
                             i32(), {ptrTy(), ptrTy()});
    llvm::Value* isStr = builder_.CreateCall(
        isStrFn, {recvInThunk, vtableTablePtr()}, "objthunk.isstr");
    auto* strBB = llvm::BasicBlock::Create(
        llvm_, "objthunk.str", thunk);
    auto* vtBB = llvm::BasicBlock::Create(
        llvm_, "objthunk.vt", thunk);
    builder_.CreateCondBr(
        builder_.CreateICmpNE(isStr, builder_.getInt32(0)),
        strBB, vtBB);
    // string receiver → content semantics (+ retain for ToString).
    builder_.SetInsertPoint(strBB);
    llvm::Value* strRes = nullptr;
    llvm::BasicBlock* strEnd = nullptr;
    if (fixedSlot == kToStringSlot) {
        strRes = recvInThunk;
        emitStringRetain(strRes);
        builder_.CreateBr(endBB);
        strEnd = builder_.GetInsertBlock();
    } else if (fixedSlot == kEqualsSlot) {
        // §9.6: a string receiver equals another iff other is also a string
        // of equal content (the #9.6 `string.Equals(Object?)` overload).
        // The string arm splits into TWO sub-arms so the outer PHI receives
        // a distinct predecessor for each — content-equality vs always-false.
        // Both sub-arms unconditionally branch to endBB, and the outer PHI
        // (built below) receives four incoming entries (null, strEq, strNe,
        // vt) instead of the three the ToString / GetHashCode arms use.
        auto* strEqBB = llvm::BasicBlock::Create(
            llvm_, "objthunk.streq", thunk);
        auto* strNeBB = llvm::BasicBlock::Create(
            llvm_, "objthunk.strne", thunk);
        llvm::Value* otherIsStr = builder_.CreateCall(
            isStrFn, {otherArg, vtableTablePtr()}, "objthunk.oisstr");
        builder_.CreateCondBr(
            builder_.CreateICmpNE(otherIsStr, builder_.getInt32(0)),
            strEqBB, strNeBB);
        builder_.SetInsertPoint(strEqBB);
        llvm::Value* strEqRes = emitStringEq(recvInThunk, otherArg);
        // emitStringEq may emit its own basic blocks; capture the block the
        // intrinsic left the builder in as the predecessor of endBB.
        llvm::BasicBlock* strEqEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(strNeBB);
        llvm::Value* strNeRes = builder_.getFalse();
        builder_.CreateBr(endBB);
        // Park the two string-arm PHI predecessors in a side vector; the
        // outer PHI below (built unconditionally for all three methods)
        // splices them in instead of the ToString/GetHashCode's single
        // strEnd when fixedSlot == kEqualsSlot.
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(retTy, 4, "objthunk.res");
        phi->addIncoming(nullRes, nullEnd);
        phi->addIncoming(strEqRes, strEqEnd);
        phi->addIncoming(strNeRes, strNeBB);
        // vtEnd is filled in below by the vtable arm.
        llvm::PHINode* eqPhi = phi;
        // class/box receiver → dispatch vtable[fixed slot].
        builder_.SetInsertPoint(vtBB);
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recvInThunk);
        llvm::Value* vtFn = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), vptr,
                               {builder_.getInt32(fixedSlot)}),
            "objthunk.vfn");
        std::vector<llvm::Type*> vtParamTys{ptrTy()};
        if (takesOther) vtParamTys.push_back(ptrTy());
        auto* vtFnTy = llvm::FunctionType::get(retTy, vtParamTys, false);
        std::vector<llvm::Value*> vtCallArgs{recvInThunk};
        if (takesOther) vtCallArgs.push_back(otherArg);
        llvm::Value* vtRes = builder_.CreateCall(
            vtFnTy, vtFn, vtCallArgs, "objthunk.r");
        if (vtRes->getType() != retTy) {
            if (vtRes->getType()->isIntegerTy() && retTy->isIntegerTy())
                vtRes = builder_.CreateZExtOrTrunc(vtRes, retTy, "objthunk.z");
        }
        llvm::BasicBlock* vtEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        // Now the vtable arm has its terminator; the endBB block was created
        // earlier as a forward declaration but has no terminator yet — we are
        // currently in vtBB after the CreateBr above, and need to install a
        // terminator for endBB. The unified PHI was already built above; its
        // predecessor list is correct. Create a Ret directly from endBB to
        // close the function body.
        builder_.SetInsertPoint(endBB);
        eqPhi->addIncoming(vtRes, vtEnd);
        if (retTy->isVoidTy()) builder_.CreateRetVoid();
        else builder_.CreateRet(eqPhi);
        // Restore emitter state and return (the ToString/GetHashCode arms
        // fall through below with their own PHI + return).
        currentFn_ = savedFn;
        currentThis_ = savedThis;
        locals_ = std::move(savedLocals); paramSlots_ = std::move(savedParams);
        refParams_ = std::move(savedRefParams);
        disposeStack_ = std::move(savedDisposeStack);
        loopStack_ = std::move(savedLoopStack);
        finallyStack_ = std::move(savedFinallyStack);
        finallyBodyStack_ = std::move(savedFinallyBodies);
        unwindTarget_ = savedUnwind; propagateBB_ = savedProp;
        currentCaughtExc_ = savedCaught;
        unwindScopeDepth_ = savedUnwindDepth;
        diverged_ = savedDiverged;
        if (savedBB) builder_.SetInsertPoint(savedBB);
        return buildMethodClosureCell(thunk, /*instMethod=*/true,
                                      /*receiverVal=*/recvVal, FVK_FN_PLUS_RECV);
    } else {
        sema::TypeRef strTy;
        strTy.kind = sema::TypeRef::Kind::Primitive;
        strTy.primitive = sema::PrimitiveType::String;
        strRes = emitValueHashCode(recvInThunk, &strTy);
        if (strRes->getType()->isIntegerTy() &&
            !strRes->getType()->isIntegerTy(32))
            strRes = builder_.CreateZExtOrTrunc(
                strRes, i32(), "objthunk.hash");
        builder_.CreateBr(endBB);
        strEnd = builder_.GetInsertBlock();
    }
    // class/box receiver → dispatch vtable[fixed slot].
    builder_.SetInsertPoint(vtBB);
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recvInThunk);
    llvm::Value* vtFn = builder_.CreateLoad(
        ptrTy(),
        builder_.CreateGEP(ptrTy(), vptr,
                           {builder_.getInt32(fixedSlot)}),
        "objthunk.vfn");
    // Build the vtable call type. The slot's callee signature is
    // `T(ptr this [, ptr other])` for classHashCodeThunk /
    // classEqualsThunk / classToStringThunk.
    std::vector<llvm::Type*> vtParamTys{ptrTy()};
    if (takesOther) vtParamTys.push_back(ptrTy());
    auto* vtFnTy = llvm::FunctionType::get(retTy, vtParamTys, false);
    std::vector<llvm::Value*> vtCallArgs{recvInThunk};
    if (takesOther) vtCallArgs.push_back(otherArg);
    llvm::Value* vtRes = builder_.CreateCall(
        vtFnTy, vtFn, vtCallArgs, "objthunk.r");
    if (vtRes->getType() != retTy) {
        if (vtRes->getType()->isIntegerTy() && retTy->isIntegerTy())
            vtRes = builder_.CreateZExtOrTrunc(vtRes, retTy, "objthunk.z");
    }
    llvm::BasicBlock* vtEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);
    // Merge the three arms with a retTy-keyed PHI.
    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(retTy, 3, "objthunk.res");
    phi->addIncoming(nullRes, nullEnd);
    phi->addIncoming(strRes, strEnd);
    phi->addIncoming(vtRes, vtEnd);
    if (retTy->isVoidTy()) builder_.CreateRetVoid();
    else builder_.CreateRet(phi);
    // Restore emitter state (mirrors makeMethodThunk's tail).
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals); paramSlots_ = std::move(savedParams);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);
    finallyBodyStack_ = std::move(savedFinallyBodies);
    unwindTarget_ = savedUnwind; propagateBB_ = savedProp;
    currentCaughtExc_ = savedCaught;
    unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    if (savedBB) builder_.SetInsertPoint(savedBB);
    // Build the closure cell: { thunk, env, rc, envdtor, kind }. The env
    // holds the captured receiver (§28.3 borrow). instMethod=true so the
    // cell gets an env block; receiverVal is the SSA value of the
    // receiver at the use site, which buildMethodClosureCell copies into
    // the heap env block.
    return buildMethodClosureCell(thunk, /*instMethod=*/true,
                                  /*receiverVal=*/recvVal, FVK_FN_PLUS_RECV);
}

// §27.6 (v0.184, #1758): back a bare reference to a primitive-intrinsic static
// method (`int.Parse`, `char.IsDigit`, `double.IsNaN`, …) with a thunk closure.
// See the header declaration for the full contract. A primitive intrinsic has no
// MethodSymbol (it is name-recognized in sema/codegen), so makeMethodThunk cannot
// back it; the SIGSEGV of #1758 was the CG60 method-ref path returning nullptr
// for such a reference — a null fn pointer the lazy pipeline then invoked.
llvm::Value* Emitter::makePrimIntrinsicThunk(const ast::MemberAccessExpr& ma,
                                             const sema::TypeRef* fnTy) {
    if (!fnTy || fnTy->kind != sema::TypeRef::Kind::Function) return nullptr;
    auto* id = ma.target
        ? dynamic_cast<const ast::IdentifierExpr*>(ma.target.get()) : nullptr;
    if (!id) return nullptr;
    sema::PrimitiveType pt = sema::primitiveFromName(id->name);
    if (pt == sema::PrimitiveType::None) return nullptr;

    // Thunk signature: ret (ptr env, params…) — the closure ABI the LINQ
    // trampolines and the general function-value call site expect (fn(env, args)).
    // env is null for this static reference (no captures); the thunk ignores it.
    llvm::Type* retTy = fnTy->funcReturn ? lowerTypeRef(fnTy->funcReturn)
                                         : llvm::Type::getVoidTy(llvm_);
    std::vector<llvm::Type*> thunkParamTys;
    thunkParamTys.push_back(ptrTy());  // env (ignored)
    for (const auto& fp : fnTy->funcParams)
        thunkParamTys.push_back(fp.type ? lowerTypeRef(fp.type) : ptrTy());
    auto* thunkFnTy = llvm::FunctionType::get(retTy, thunkParamTys, false);

    // Reuse a cached thunk per (kind, member) so repeated references share ONE fn
    // pointer — a static reference is FVK_FN_ONLY, so §27.7.1 equality is by fn
    // alone (the #1170 stability reasoning applied to primitive intrinsics).
    std::pair<int, std::string> key{static_cast<int>(pt), std::string(ma.member)};
    if (auto cit = primIntrinsicThunks_.find(key);
        cit != primIntrinsicThunks_.end())
        return buildMethodClosureCell(cit->second, /*instMethod=*/false,
                                      /*receiverVal=*/nullptr, FVK_FN_ONLY);

    static std::atomic<unsigned> primThunkCtr{0};
    std::string thunkName = "primthunk$" + std::string(id->name) + "$" +
                            std::string(ma.member) + "$" +
                            std::to_string(primThunkCtr.fetch_add(1));
    auto* thunk = llvm::Function::Create(
        thunkFnTy, llvm::Function::InternalLinkage, thunkName, module_.get());

    // Save emitter state (mirror makeMethodThunk; also snapshot finallyStack_).
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    llvm::BasicBlock* savedBB = builder_.GetInsertBlock();
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    auto savedFinallyStack = finallyStack_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::BasicBlock* savedProp = propagateBB_;
    llvm::Value* savedCaught = currentCaughtExc_;
    std::size_t savedUnwindDepth = unwindScopeDepth_;
    bool savedDiverged = diverged_;

    currentFn_ = thunk;
    currentThis_ = nullptr;
    locals_.clear(); paramSlots_.clear(); refParams_.clear();
    disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
    unwindTarget_ = nullptr; propagateBB_ = nullptr;
    currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0; diverged_ = false;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", thunk));

    // Build a synthetic `Type.Member(args…)` call whose arguments lower to the
    // thunk's incoming parameters, then lower it through the SAME intrinsic path a
    // direct call uses (tryLowerPrimitiveStaticCall). This reuses every primitive-
    // static member's existing lowering — no per-member thunk emitter. The
    // synthetic nodes reuse the original source-backed string_views (id->name,
    // ma.member); the tree is owned by primIntrinsicSynthCalls_ so the exprType
    // set on each arg node below never outlives its key.
    auto synthCall = std::make_unique<ast::CallExpr>();
    {
        auto synthTargetId = std::make_unique<ast::IdentifierExpr>();
        synthTargetId->name = id->name;
        auto synthCallee = std::make_unique<ast::MemberAccessExpr>();
        synthCallee->target = std::move(synthTargetId);
        synthCallee->member = ma.member;
        synthCall->callee = std::move(synthCallee);
    }
    std::vector<const ast::Expr*> synthArgs;
    for (std::size_t i = 0; i < fnTy->funcParams.size(); ++i) {
        ast::CallArgument arg;
        auto argExpr = std::make_unique<ast::IdentifierExpr>();
        argExpr->name = ma.member;  // unused; the value comes from the override
        // Thunk arg 0 is env; user params start at 1.
        exprValueOverride_[argExpr.get()] = thunk->getArg(static_cast<unsigned>(i) + 1);
        // Some intrinsic handlers (numeric Parse) inspect the arg's exprType to
        // select the string-parse branch — give the synthetic arg its param type.
        if (fnTy->funcParams[i].type)
            ctx_.setExprType(
                argExpr.get(),
                const_cast<sema::TypeRef*>(fnTy->funcParams[i].type));
        synthArgs.push_back(argExpr.get());
        arg.value = std::move(argExpr);
        synthCall->args.push_back(std::move(arg));
    }

    llvm::Value* result = tryLowerPrimitiveStaticCall(*synthCall);

    // Drop the per-arg value overrides now that the body is emitted; keep the
    // tree (and its exprType keys) alive by parking it in the owner vector.
    for (const ast::Expr* a : synthArgs) exprValueOverride_.erase(a);
    primIntrinsicSynthCalls_.push_back(std::move(synthCall));

    if (!result && !retTy->isVoidTy()) result = llvm::PoisonValue::get(retTy);
    if (retTy->isVoidTy()) builder_.CreateRetVoid();
    else builder_.CreateRet(result);

    // Restore emitter state.
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals); paramSlots_ = std::move(savedParams);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);
    unwindTarget_ = savedUnwind; propagateBB_ = savedProp;
    currentCaughtExc_ = savedCaught; unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    if (savedBB) builder_.SetInsertPoint(savedBB);

    primIntrinsicThunks_[key] = thunk;
    return buildMethodClosureCell(thunk, /*instMethod=*/false,
                                  /*receiverVal=*/nullptr, FVK_FN_ONLY);
}

// §27.6 (v0.184, #1758): see the header declaration. Recognizes the bare
// primitive-intrinsic method-reference VALUE shape that makePrimIntrinsicThunk
// backs with a fresh +1 closure cell — kept in one place so every producer/
// consumer site agrees on the recognized set.
bool Emitter::isPrimIntrinsicMethodRef(const ast::Expr* e) const {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e);
    if (!ma || !ma->target) return false;
    if (ctx_.exprSymbol(*e)) return false;          // a real MethodSymbol ref
    const sema::TypeRef* t = ctx_.exprType(*e);      // sema typed it as a value
    if (!t || t->kind != sema::TypeRef::Kind::Function) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
    return id && sema::primitiveFromName(id->name) != sema::PrimitiveType::None;
}

// fix #2781 (§8.1 / §14.2 / §27.6): mirror of isPrimIntrinsicMethodRef for
// the Object-receiver path. Recognizes `var f := o.ToString;` /
// `o.Equals` / `o.GetHashCode;` (o: Object). The CG60 gate that produced
// the closure cell sets exprType to Function and exprSymbol to null (no
// MethodSymbol exists for the universal Object members — they live in the
// fixed vtable slots 3/4/5 and are dispatched structurally by codegen), so
// both conditions are necessary, but they are not jointly sufficient — the
// same shape with a different member name (e.g. `o.EqualsOrWhatever`)
// should NOT be eligible, because the CG60 producer skipped it.
bool Emitter::isObjectMemberMethodRef(const ast::Expr* e) const {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e);
    if (!ma || !ma->target) return false;
    if (ctx_.exprSymbol(*e)) return false;          // a real MethodSymbol ref
    const sema::TypeRef* t = ctx_.exprType(*e);      // sema/CG60 typed it
    if (!t || t->kind != sema::TypeRef::Kind::Function) return false;
    // CG60 patches the receiver's type checks (Object primitive); we do the
    // same check on the side that asks whether the producer ran.
    const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || recvTy->kind != sema::TypeRef::Kind::Primitive ||
        recvTy->primitive != sema::PrimitiveType::Object) return false;
    auto m = ma->member;
    return m == "ToString" || m == "Equals" || m == "GetHashCode";
}

// §27.7.1 (#1544): classify the equality kind of a method-reference function
// value. The runtime cannot tell a class receiver (which must match by fn +
// receiver across separate evaluations) from a record receiver (which must NOT,
// having no identity), so the kind is tagged at the cell and dispatched on by
// bz_closure_value_equals / bz_closure_same.
Emitter::FnValKind Emitter::closureKindForMethod(sema::MethodSymbol* ms,
                                                 bool extRef) {
    // Static, or an UNBOUND extension reference (receiver is the function's
    // first parameter, never captured in env): no receiver to match — same fn.
    if (isStaticMethod(ms) || extRef) return FVK_FN_ONLY;
    // Bound extension reference: classify by the extension's TARGET type kind.
    if (isExtensionMethod(ms)) {
        if (ms->extensionDecl && ms->extensionDecl->targetType) {
            const sema::TypeRef* rt =
                ctx_.resolvedType(*ms->extensionDecl->targetType);
            if (rt && rt->kind == sema::TypeRef::Kind::Named &&
                rt->declaration) {
                auto k = rt->declaration->kind;
                if (k == ast::TypeDeclKind::Class ||
                    k == ast::TypeDeclKind::Interface)
                    return FVK_FN_PLUS_RECV;
            }
        }
        // record / enum / primitive extension receiver — value-shaped: no
        // identity; two separate evaluations hold distinct copies.
        return FVK_REF_IDENTITY;
    }
    // Regular instance method: classify by the declaring type's kind.
    if (ms->parent) {
        auto k = ms->parent->kind;
        if (k == ast::TypeDeclKind::Class ||
            k == ast::TypeDeclKind::Interface)
            return FVK_FN_PLUS_RECV;
    }
    // record / enum instance receiver — value-shaped: equals only itself.
    return FVK_REF_IDENTITY;
}

llvm::Value* Emitter::buildMethodClosureCell(llvm::Function* thunk,
                                             bool instMethod,
                                             llvm::Value* receiverVal,
                                             FnValKind kind) {
    // Build closure cell { thunk_ptr, env_ptr, rc, envdtor, kind }.
    // #422 / #672 / §27.6 / §28.9 / §27.7.1 (#1544): a method reference is a
    // runtime-managed function value backed by the same closure cell as a lambda
    // (40 bytes: rc at off 16, envdtor at off 24, kind at off 32). The env is
    // null for a static method, or an 8-byte block holding the receiver (a
    // BORROW of the receiver, §28.3 — the cell does not own it). bz_closure_
    // release frees the cell + env block, never the receiver. envdtor is null
    // (foundation: the env for a method thunk holds a borrow and needs no typed
    // disposal). kind tags the §27.7.1 equality rule (set below).
    // For a primitive/enum extension receiver the env block stores the raw value
    // bytes (≤ 8 bytes for all supported primitives).
    // #1170: a FRESH cell+env is built per reference (the thunk fn is shared via
    // the cache), so each reference still has its own rc/lifetime; equality is
    // by the cell's { fn, receiver-in-env } value (bz_event_remove_closure).
    uint64_t cellSz =
        module_->getDataLayout().getTypeAllocSize(closureCellType());
    llvm::Value* closureObj = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), builder_.getInt64((int64_t)cellSz)});
    builder_.CreateStore(thunk, closureObj);
    // For instance methods: store a copy of the receiver in env.
    llvm::Value* envObj = llvm::ConstantPointerNull::get(ptrTy());
    if (instMethod && receiverVal) {
        // §26.1 / #457: size the env block to fit the actual receiver value.
        // For pointer/record receivers this is always 8 bytes (one ptr).
        // For primitive/enum receivers (e.g. i32) the data layout gives the
        // true size; allocate that many bytes so the store + load are safe.
        uint64_t envSz = module_->getDataLayout()
                             .getTypeStoreSize(receiverVal->getType())
                             .getFixedValue();
        if (envSz < 8) envSz = 8;  // minimum 8 for alignment
        envObj = builder_.CreateCall(
            callocFn(), {builder_.getInt64(1), builder_.getInt64((int64_t)envSz)});
        builder_.CreateStore(receiverVal, envObj);
    }
    builder_.CreateStore(
        envObj,
        builder_.CreateGEP(ptrTy(), closureObj, {builder_.getInt32(1)}));
    initClosureRc(closureObj);      // #422: producer's reference, rc := 1
    initClosureEnvdtor(closureObj); // #672: foundation null envdtor
    initClosureKind(closureObj, kind); // §27.7.1 (#1544): equality-rule tag
    return closureObj;
}


// §27.6 (#756): a PROPERTY reference used in function-value position
// (`var f: () -> int := b.Count`) produces the getter as a function value —
// equivalent to `() => b.Count`. Build a closure { thunk_ptr, env } whose thunk
// loads the captured receiver from env and computes the property getter (the
// same getter the normal `b.Count` read would, dispatched virtually when the
// property is virtual/interface). Mirrors makeMethodThunk; the getter takes no
// user parameters. `recv` is a BORROW of the receiver (the cell does not own it).
llvm::Value* Emitter::makePropertyThunk(sema::PropertySymbol* ps,
                                        llvm::Value* receiverVal,
                                        const ast::MemberAccessExpr* ma) {
    if (!ps) return nullptr;
    // §27.7.1 (v0.180, #1544): reuse a cached thunk for this property so a
    // named property reference resolves to a STABLE fn pointer (the cell's fn
    // at offset 0) — the same #1170 reasoning makeMethodThunk applies below,
    // needed so two references to the SAME property on the SAME receiver
    // compare EQUAL under the general `=`/`<>` operator (bz_closure_value_
    // equals / §27.7.1 fourth bullet: "property references follow the
    // method-reference rule"). On a cache hit skip re-emitting the body (the
    // property's return type / generic substitution were already baked into
    // the compiled thunk on first emission) and go straight to building a
    // fresh closure cell with the current receiver.
    // §27.7.1 (#1544): classify the equality kind once and thread it into
    // buildPropertyClosureCell on both paths (mirrors makeMethodThunk).
    FnValKind pkind = closureKindForProperty(ps, receiverVal);
    if (auto cit = propertyThunks_.find(ps); cit != propertyThunks_.end())
        return buildPropertyClosureCell(cit->second, receiverVal, pkind);
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    // Result type = the property's value type.
    llvm::Type* retTy = nullptr;
    if (pd && pd->type) retTy = lowerType(pd->type.get());
    else if (sema::TypeRef* tr = ctx_.importedMemberType(ps)) retTy = lowerTypeRef(tr);
    if (!retTy) retTy = i32();

    // Thunk fn type: retTy thunk(ptr env). A getter takes no user params.
    auto* thunkFnTy = llvm::FunctionType::get(retTy, {ptrTy()}, false);
    static std::atomic<unsigned> pthunkCtr{0};
    std::string thunkName = "pthunk$" + std::to_string(pthunkCtr.fetch_add(1));
    auto* thunk = llvm::Function::Create(
        thunkFnTy, llvm::Function::InternalLinkage, thunkName, module_.get());

    // Save emitter state (mirror makeMethodThunk).
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    llvm::BasicBlock* savedBB = builder_.GetInsertBlock();
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    // #2281 / §31.2: the property thunk is a separate function. Preserve the
    // caller's active try/finally frames across this nested emission; keep the
    // parallel finally-body stack depth-aligned per §29.7.
    auto savedFinallyStack = finallyStack_;
    auto savedFinallyBodies = finallyBodyStack_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::BasicBlock* savedProp = propagateBB_;
    llvm::Value* savedCaught = currentCaughtExc_;
    std::size_t savedUnwindDepth = unwindScopeDepth_;
    bool savedDiverged = diverged_;
    auto savedSubst = genericSubst_;
    auto savedSemaTy = genericSubstSemaTy_;

    currentFn_ = thunk;
    currentThis_ = nullptr;
    locals_.clear(); paramSlots_.clear(); refParams_.clear();
    disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
    finallyBodyStack_.clear();
    unwindTarget_ = nullptr; propagateBB_ = nullptr;
    currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0; diverged_ = false;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", thunk));

    // fix #2501 (§27.6, v0.236): "the instance is captured at formation — by
    // reference for class-typed receivers, by copy for value-shaped (record)
    // receivers". buildPropertyClosureCell (below) now copies ANY receiver
    // shape into env, sized by getTypeStoreSize (mirroring
    // buildMethodClosureCell) — so the reload here must match that shape, not
    // assume a pointer. The shape is read off `receiverVal`'s OWN LLVM type
    // (produced by the caller's `lowerExpr` on the receiver expression, so it
    // is already correctly specialized for a generic receiver — no dependency
    // on the §14.1 genericSubst_ block below) rather than re-derived from
    // `ps->parent`, so this also covers an EXTENSION property (whose
    // PropertySymbol carries no `parent` — SymbolCollection.cpp "patched by
    // slice 2" never patches it for properties, only `MethodSymbol::
    // extensionDecl` gets the analogous method-side wiring). The thunk body
    // is built ONCE and cached per `ps` (propertyThunks_), so this shape is
    // fixed at first emission — the same per-symbol-not-per-instantiation
    // caching methodThunks_ already accepts.
    //   - CLASS/INTERFACE receiver (or no receiver at all — a static
    //     property; `thisVal` then has no real use since a static getter
    //     body never reads `this`): unchanged pointer load.
    //   - RECORD receiver: env holds the struct VALUE (an aggregate,
    //     non-pointer type). Reload it, then re-spill to a fresh alloca so
    //     the getter body's `this.field` GEPs still see a pointer — mirrors
    //     the ordinary (non-thunk) record-getter read path
    //     (LlvmBackend_Exprs.cpp, "issue #347 / §9.2").
    //   - ENUM (or primitive-extension) receiver: env holds the raw SCALAR
    //     value (e.g. i32 — enumLlvmType). Reload it as-is and use it
    //     directly as `this` — never spill to a pointer, mirroring CG30 /
    //     the §10.4 `isEnumReceiver` carve-out in that same read path (a
    //     `case this` arm needs the bare scalar to icmp against an enum
    //     constant; comparing a spilled pointer against one is malformed IR).
    llvm::Value* envArg = thunk->getArg(0);
    llvm::Type* recvLlvmTy = receiverVal ? receiverVal->getType() : nullptr;
    llvm::Value* thisVal;
    if (!recvLlvmTy || recvLlvmTy->isPointerTy()) {
        thisVal = builder_.CreateLoad(ptrTy(), envArg);
    } else if (recvLlvmTy->isAggregateType()) {
        llvm::Value* loadedRecv = builder_.CreateLoad(recvLlvmTy, envArg);
        llvm::AllocaInst* recvTmp = entryAlloca(recvLlvmTy);
        builder_.CreateStore(loadedRecv, recvTmp);
        thisVal = recvTmp;
    } else {
        thisVal = builder_.CreateLoad(recvLlvmTy, envArg);
    }
    currentThis_ = thisVal;

    // §14.1: for a generic class receiver, set up the generic substitution so a
    // getter body that reads a `T`-typed field uses the right struct layout.
    // (Mirror the computed-getter read path in LlvmBackend_Exprs.cpp.)
    if (ma && ma->target && ps->parent) {
        if (sema::TypeRef* recvTy = ctx_.exprType(*ma->target)) {
            if (recvTy->kind == sema::TypeRef::Kind::Named &&
                recvTy->declaration == ps->parent && !recvTy->genericArgs.empty()) {
                // fix #3143 (§14.1 / §2.5, v0.254): parameter names through the
                // one both-sources walk (and the `parent->decl` pre-guard
                // dropped with it), mirroring the read path in
                // LlvmBackend_Exprs.cpp this block is the mirror of.
                const std::vector<std::string_view> gpNames =
                    sema::genericParamNames(ps->parent);
                if (!gpNames.empty() &&
                    gpNames.size() == recvTy->genericArgs.size()) {
                    std::unordered_map<std::string_view, llvm::Type*> newSubst;
                    std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
                    for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                        newSubst[gpNames[gi]] =
                            lowerTypeRef(recvTy->genericArgs[gi]);
                        newSema[gpNames[gi]] =
                            resolveTypeParamRef(recvTy->genericArgs[gi]);
                    }
                    genericSubst_ = std::move(newSubst);
                    genericSubstSemaTy_ = std::move(newSema);
                }
            }
        }
    }

    // Compute the getter result. Virtual/interface properties dispatch through
    // the receiver's vtable/itable; otherwise evaluate the concrete getter.
    llvm::Value* result = nullptr;
    bool isIfaceProp = ps->parent &&
                       ps->parent->kind == ast::TypeDeclKind::Interface;
    auto getIt = propGetSlot_.find(ps);
    auto ifGetIt = ifacePropertyGetIndex_.find(ps);
    bool dynamicGet = thisVal->getType()->isPointerTy() &&
        (getIt != propGetSlot_.end() || ifGetIt != ifacePropertyGetIndex_.end());
    if (dynamicGet) {
        auto* fnTy = llvm::FunctionType::get(retTy, {ptrTy()}, false);
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), thisVal);
        llvm::Value* fnPtr = nullptr;
        if (isIfaceProp && ifGetIt != ifacePropertyGetIndex_.end()) {
            auto descIt = descArray_.find(ps->parent);
            if (descIt != descArray_.end()) {
                llvm::Value* itmapPtr = builder_.CreateLoad(
                    ptrTy(),
                    builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(1)}));
                auto* lookupFn = declareC("bz_itable_lookup", ptrTy(),
                                          {ptrTy(), ptrTy()});
                llvm::Value* itab = builder_.CreateCall(
                    lookupFn, {itmapPtr, descIt->second}, "pthunk.itab");
                fnPtr = builder_.CreateLoad(
                    ptrTy(),
                    builder_.CreateGEP(ptrTy(), itab,
                                       {builder_.getInt32(ifGetIt->second)}),
                    "pthunk.ifn");
            }
        } else if (getIt != propGetSlot_.end()) {
            fnPtr = builder_.CreateLoad(
                ptrTy(),
                builder_.CreateGEP(ptrTy(), vptr,
                                   {builder_.getInt32(getIt->second)}),
                "pthunk.vfn");
        }
        if (fnPtr) {
            result = builder_.CreateCall(fnTy, fnPtr, {thisVal}, "pthunk.r");
            emitPendingCheck();
        }
    }
    if (!result) {
        if (const ast::Expr* getter = computedGetterOf(ps)) {
            // fix #2501 (#574 / §10.4 / §19.2 precedent): an enum computed
            // property's getter body commonly dispatches with `case this ...`
            // — patternMatches needs enumTypeOf("this") to resolve the
            // declaring enum type, and enumTypeOf falls back to
            // currentEnumMethod_ for a bare `this` identifier because sema
            // attaches no exprType to it. The ordinary (non-thunk) getter
            // read path sets this (LlvmBackend_Exprs.cpp, the #574 fix);
            // makePropertyThunk never did, so an enum property referenced as
            // a function value with a `case this` body was rejected at
            // codegen ("pattern … did not resolve to a type, enum constant,
            // or named constant") even after the receiver-capture fix above.
            const sema::TypeSymbol* savedEnumMethod = currentEnumMethod_;
            if (ps->parent && ps->parent->kind == ast::TypeDeclKind::Enum)
                currentEnumMethod_ = ps->parent;
            // fix #4113 (§35.2/§35.5, v0.295): the choice twin of the enum
            // workaround above — a choice property read as a function value
            // (`var f: () -> T := s.SomeProperty;`) whose getter body is
            // `case this of …` needs the identical choiceTypeOf() recovery.
            //
            // fix #4143 (§35.2/§35.5, v0.295): SET-FROM-OWNER, not
            // conditionally-set-else-inherit — this thunk switches
            // `currentFn_` to a genuinely separate, memoized function
            // (`pthunk$N`, `propertyThunks_`) via its OWN hand-rolled
            // save/restore above, NOT the `NestedBodyScope` guard (this
            // function predates it and is not one of the six builders that
            // guard wraps), so an unconditional-else-untouched write here
            // leaks a caller's `currentChoiceMethod_` into this thunk's body
            // exactly as the inline read site did (LlvmBackend_Exprs.cpp) —
            // same mechanism, different call site.
            const sema::TypeSymbol* savedChoiceMethod = currentChoiceMethod_;
            currentChoiceMethod_ =
                (ps->parent && ps->parent->kind == ast::TypeDeclKind::Choice)
                    ? ps->parent
                    : nullptr;
            result = lowerExpr(*getter);
            // fix #2761 (§8.6 "Boxing" / §27.6, v0.240): the PROPERTY-AS-
            // FUNCTION-VALUE face of the same expression getter. Its `ret` is
            // typed from `pd->type` a few lines above, so a value-typed body
            // under an `Object`/interface declared type produced `ret i32` from
            // a `ptr`-returning thunk — a hard build failure, reproduced on
            // 0e554e3 with `var f: () -> Object := h.O; f()`. Same box, same
            // reasoning as the inlined read (LlvmBackend_Exprs.cpp).
            if (pd && pd->type)
                result = boxForTarget(result, ctx_.resolvedType(*pd->type),
                                      getter);
            currentEnumMethod_ = savedEnumMethod;
            currentChoiceMethod_ = savedChoiceMethod;
        } else if (const ast::PropertyAccessor* acc = blockGetterOf(ps)) {
            if (llvm::Function* fn = ensurePropertyBlockGetter(pd, acc)) {
                // fix #2501 (§9.2/§27.6, v0.236): ensurePropertyBlockGetter's
                // generated function always takes `this` as a POINTER
                // (`fnTy = (ptr) -> retTy`, its own `currentThis_ = &*fn->
                // args().begin()`), mirroring the ordinary (non-thunk)
                // block-getter read path's unconditional spill
                // (LlvmBackend_Exprs.cpp, "issue #347 / §9.2" — spills even
                // an enum/primitive `this`, unlike the expression-getter
                // path's isEnumReceiver carve-out just above). `thisVal` here
                // is the raw scalar for an enum/primitive receiver (by
                // design, so `case this` above can icmp it directly) — spill
                // it to a fresh pointer for this call site specifically,
                // rather than passing the scalar where a pointer is expected
                // (a thunk->fn signature mismatch / malformed-IR call).
                llvm::Value* blockThis = thisVal;
                if (!blockThis->getType()->isPointerTy()) {
                    llvm::AllocaInst* blockTmp =
                        entryAlloca(blockThis->getType());
                    builder_.CreateStore(blockThis, blockTmp);
                    blockThis = blockTmp;
                }
                result = builder_.CreateCall(fn->getFunctionType(), fn,
                                             {blockThis});
                emitPendingCheck();
            }
        } else if (sema::isAutoPropertyStorageSymbol(ps, ctx_)) {
            // fix #2478 (§9.2/§22.8): isAutoPropertyStorageSymbol, not the bare
            // decl-dependent hasAstAutoPropertyDecl — an imported auto-property
            // referenced as a FUNCTION VALUE (`var f := obj.Prop;`, §27.6)
            // fell through to the `llvm::Constant::getNullValue` default
            // below without this, silently yielding a permanently-0 thunk.
            auto idxIt = fieldIndex_.find(ps);
            if (idxIt != fieldIndex_.end()) {
                llvm::StructType* st = classStruct(ps->parent);
                llvm::Type* fty = st->getElementType(idxIt->second);
                result = builder_.CreateLoad(
                    fty, builder_.CreateStructGEP(st, thisVal, idxIt->second));
            }
        } else if (ps->parent &&
                   ps->parent->kind == ast::TypeDeclKind::Class) {
            // fix #2489 (§9.2/§27.6, v0.236): an imported, non-virtual,
            // non-auto COMPUTED property (an arrow-getter or block-form
            // getter with no ast::PropertyDecl to drive computedGetterOf/
            // blockGetterOf above, and no storage slot for
            // isAutoPropertyStorageSymbol's branch either) referenced as a
            // FUNCTION VALUE fell through every branch above straight to
            // the `getNullValue` default below — silently yielding a
            // thunk that always returns 0, never calling anything.
            // Confirmed by probe: `var fn: () -> int := obj.Getter;
            // return fn();` against an imported CLASS's arrow-getter
            // property returned 0 instead of the field's real value.
            // Mirror the ordinary (non-thunk) imported-getter fallback
            // (LlvmBackend_Exprs.cpp's "imported non-virtual property
            // getter" site, issue #503/#2474): call the exported
            // `$propget` stub declareImportedPropertyGetters declared.
            // `thisVal` here is ALWAYS `ptr` because this branch's own guard
            // (`ps->parent->kind == Class`, just above) excludes Record/Enum
            // — no by-value spill is needed the way the ordinary read path
            // needs one.
            //
            // UPDATE (fix #2501, §27.6 v0.236): the general bug this
            // comment originally described — buildPropertyClosureCell left
            // the env null for any non-pointer (Record/Enum) receiver, so
            // `thisVal` would have been a null pointer here had this branch
            // admitted one — is now fixed at its source: buildPropertyClosureCell
            // captures ANY receiver shape (sized by getTypeStoreSize,
            // mirroring buildMethodClosureCell), and the `this`-reconstruction
            // a few lines above this function's start now branches on that
            // shape (pointer / spilled-record-pointer / raw-enum-scalar), so
            // a LOCAL record or enum arrow-getter referenced as a function
            // value no longer SIGSEGVs (see makePropertyThunk's `this`-
            // reconstruction comment). This branch's restriction to Class is
            // kept AS-IS regardless: it exists for the CROSS-MODULE ABI
            // surface (the imported `$propget` stub call below), which
            // #2501 did not exercise or test end-to-end (no `importedPropGetterFns_`
            // entry was involved in its repro) — widening it to Record/Enum
            // is a #2489-shaped follow-up, not folded in here.
            auto git = importedPropGetterFns_.find(ps);
            if (git != importedPropGetterFns_.end()) {
                llvm::Function* fn = git->second;
                result = builder_.CreateCall(fn->getFunctionType(), fn,
                                             {thisVal});
                emitPendingCheck();
            }
        }
    }
    if (!result) result = llvm::Constant::getNullValue(retTy);
    builder_.CreateRet(result);

    // Restore emitter state.
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals); paramSlots_ = std::move(savedParams);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);          // #2281 / §31.2
    finallyBodyStack_ = std::move(savedFinallyBodies);     // #2281 / §29.7
    unwindTarget_ = savedUnwind; propagateBB_ = savedProp;
    currentCaughtExc_ = savedCaught; unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (savedBB) builder_.SetInsertPoint(savedBB);

    // §27.7.1 (v0.180, #1544): cache the thunk so subsequent references to
    // this property (e.g. a repeated `b.Count` read as a function value)
    // reuse the SAME fn pointer — mirrors makeMethodThunk's methodThunks_.
    propertyThunks_[ps] = thunk;
    return buildPropertyClosureCell(thunk, receiverVal, pkind);
}

// §27.7.1 (#1544): classify the equality kind of a property-reference function
// value. A static property (no receiver captured) is FVK_FN_ONLY; a class /
// interface receiver is FVK_FN_PLUS_RECV; a record receiver is FVK_REF_IDENTITY
// (no identity — equals only itself). Mirrors closureKindForMethod.
Emitter::FnValKind Emitter::closureKindForProperty(sema::PropertySymbol* ps,
                                                   llvm::Value* receiverVal) {
    // A static property reference has no receiver (receiverVal is null and the
    // thunk's env is null) — §27.6: "() -> PropertyType for static properties".
    if (!receiverVal) return FVK_FN_ONLY;
    if (ps->parent) {
        auto k = ps->parent->kind;
        if (k == ast::TypeDeclKind::Class ||
            k == ast::TypeDeclKind::Interface)
            return FVK_FN_PLUS_RECV;
    }
    // record receiver — value-shaped: equals only itself (§27.7.1 third bullet).
    return FVK_REF_IDENTITY;
}

llvm::Value* Emitter::buildPropertyClosureCell(llvm::Function* thunk,
                                               llvm::Value* receiverVal,
                                               FnValKind kind) {
    // Build the closure cell { thunk_ptr, env_ptr, rc, envdtor, kind } at the
    // call site. The env holds the receiver as a BORROW (§28.3); bz_closure_
    // release frees the cell + env, never the receiver. #1544: a FRESH cell+env
    // is built per reference (the thunk fn is shared via propertyThunks_), so
    // each reference still has its own rc/lifetime; equality is by the cell's
    // { fn, receiver-in-env, kind } value (bz_closure_value_equals), mirroring
    // buildMethodClosureCell.
    //
    // fix #2501 (§27.6, v0.236): "the instance is captured at formation — by
    // reference for class-typed receivers, by copy for value-shaped (record)
    // receivers". Before this fix the env was populated ONLY when
    // `receiverVal->getType()->isPointerTy()` — true for a class/interface
    // receiver, but false for a RECORD (a by-value struct, §9.2/#347) or an
    // ENUM/primitive-extension receiver (a scalar, e.g. i32). For those the
    // env silently stayed the null-pointer default, and makePropertyThunk's
    // body unconditionally loaded `this` through it — a null-pointer read
    // that SIGSEGVs the moment the getter body actually uses the receiver
    // (confirmed: issue #2501's local-record repro). Capture ANY receiver
    // shape (not just pointers), sized by getTypeStoreSize, mirroring
    // buildMethodClosureCell's instMethod branch exactly. This is a raw
    // value copy, never a retain: a record cannot declare `owned` fields or a
    // destructor (§9.7 — "Records cannot have destructors, owned fields...
    // the record does not own what they point to"), so there is nothing for
    // the record itself to own or release, and envdtor stays null below
    // (foundation: "a closure cell holds only borrows; freeing it frees ONLY
    // the cell and its env block", bezel_runtime_refcounting.cpp) — exactly
    // the same no-retain, no-envdtor contract buildMethodClosureCell already
    // relies on for a record method-reference receiver.
    uint64_t cellSz =
        module_->getDataLayout().getTypeAllocSize(closureCellType());
    llvm::Value* closureObj = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), builder_.getInt64((int64_t)cellSz)});
    builder_.CreateStore(thunk, closureObj);
    llvm::Value* envObj = llvm::ConstantPointerNull::get(ptrTy());
    if (receiverVal) {
        uint64_t envSz = module_->getDataLayout()
                             .getTypeStoreSize(receiverVal->getType())
                             .getFixedValue();
        if (envSz < 8) envSz = 8;  // minimum 8 for alignment
        envObj = builder_.CreateCall(
            callocFn(), {builder_.getInt64(1), builder_.getInt64((int64_t)envSz)});
        builder_.CreateStore(receiverVal, envObj);
    }
    builder_.CreateStore(
        envObj,
        builder_.CreateGEP(ptrTy(), closureObj, {builder_.getInt32(1)}));
    initClosureRc(closureObj);
    initClosureEnvdtor(closureObj);
    initClosureKind(closureObj, kind); // §27.7.1 (#1544): equality-rule tag
    return closureObj;
}


sema::MemberSymbol* Emitter::storageMemberOf(sema::Symbol* sym) {
    if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym)) return fs;
    // fix #2478 (§9.2/§22.8): isAutoPropertyStorageSymbol, not the bare
    // decl-dependent hasAstAutoPropertyDecl — this is the central GEP-address
    // resolution function for a plain member-access read/write
    // (fieldAddr/implicitFieldAddr and their callers below), so leaving it
    // on the decl-dependent predicate meant an imported auto-property's
    // STORAGE SLOT could be correctly counted by collectInstanceStorage/
    // collectRecordStorage (this same issue's layout fix) while every
    // ordinary READ/WRITE of that member still fell through to a different,
    // wrong path (confirmed: `b.W` on an imported record with `public
    // property W: int := 42;` still read `0` post-layout-fix until this
    // line was widened too — the layout fix alone was necessary but not
    // sufficient). See isAutoPropertyStorageSymbol's own comment
    // (LlvmBackend_impl.hpp) for the full rationale and scope boundary.
    // fix #2931 (§11.2/§23.1/§22.3/§12.1, v0.240): a property DECLARED IN AN
    // INTERFACE has NO backing storage — not in the interface (never
    // instantiated) and not in any implementing object, whose own matching
    // member owns whatever storage exists. §11.2, verbatim: "Interface members
    // are implicitly public, have no body, and end with `;`. A class
    // implements an interface implicitly: by declaring matching public
    // members." §23.1's "The compiler generates the backing field" is the
    // AUTO-PROPERTY rule; §23.2 names the two apart in one breath —
    // "auto-properties (§23.1) and interface properties, whose accessor sets
    // are declared, not shaped".
    //
    // The two forms are SPELLED IDENTICALLY (`property P: T;`) and the parser
    // records both as `PropertyBodyKind::Auto` (Parser_Decls.cpp, the
    // interface arm's "simple signature form"), so `hasAstAutoPropertyDecl` answers
    // TRUE for an interface property — and so does the exporter's wire bit
    // (`ModuleMetadataBuilder.cpp`: `out.isAutoProperty = pd->bodyKind ==
    // Auto`), which is how an IMPORTED interface property reached the same
    // wrong answer through `isAutoPropertyStorageSymbol`'s decl-null fallback. Both
    // sources meet at this one line, so excluding here covers the local and
    // the cross-module spelling alike.
    //
    // WHAT IT COST (MEASURED on 10bd6d3). This function is "the central
    // GEP-address resolution function for a plain member-access read/write",
    // so `fieldAddr` returned a GEP into the RECEIVER at the interface
    // property's ORDINAL among the interface's own declarations — not a
    // constant 0: an interface declaring `A` then `B`, implemented by a class
    // whose first two fields are `_f0`, `_f1`, sent `ref i.B`'s write into
    // `_f1` and `ref i.A`'s into `_f0`. With FIVE interface properties and a
    // ONE-field implementer, `ref i.E` was an ASan heap-buffer-overflow ("12
    // bytes after 16-byte region"). Three `lvalueAddr` consumers reached it
    // live and silently: a `ref` argument (`z=5, v=0` where `z=0, v=5` is
    // correct), an `out` argument (`z=9, v=0`), and a BUILTIN `out`
    // (`int.TryParse("42", out i.IP)` → `z=42, v=0`).
    //
    // Declining here is what routes the whole family at once: every one of
    // those consumers already falls back to §22.3's copy-in/copy-out bracket
    // (`emitAccessorRefArgCopyIn`) the moment `lvalueAddr` returns null, and
    // that bracket drives the getter and the setter through the dispatching
    // helpers. So the gate is the PROPERTY "does this member have a storage
    // slot of its own?", not an enumeration of consumers. The concrete-type
    // spelling of each shape was already correct, because a class's
    // implementing property carries accessor bodies (`PropertyBodyKind::
    // Block`) and so was never classified as storage.
    //
    // SCOPE: only the ADDRESS-resolution question is answered here.
    // `isAutoPropertyStorageSymbol` deliberately keeps its `true` for an interface
    // property — see its own comment for the disposal-classification consumers
    // that would turn a borrow into a use-after-free if it flipped.
    if (auto* ip = dynamic_cast<sema::PropertySymbol*>(sym))
        if (ip->parent && ip->parent->kind == ast::TypeDeclKind::Interface)
            return nullptr;
    if (sema::isAutoPropertyStorageSymbol(sym, ctx_))
        return dynamic_cast<sema::PropertySymbol*>(sym);
    return nullptr;
}


llvm::Value* Emitter::implicitFieldAddr(const ast::Expr& e, llvm::Type*& elemTy) {
    if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
    auto* ms = storageMemberOf(ctx_.exprSymbol(e));
    if (!ms || !ms->parent || !currentThis_) return nullptr;
    auto idxIt = fieldIndex_.find(ms);
    // fix #2474 (§9.2/§23.1/§23.2, v0.235): `idxIt == end()` here does NOT
    // necessarily mean `ms` is a static field (the pre-fix assumption, which
    // returned null unconditionally at this point) — fieldIndex_ is
    // populated lazily, by classStruct/recordStruct's FIRST call for
    // ms->parent. A local CLASS always has its struct built early
    // (declareVtables calls classStruct for every class up front to lay out
    // the vtable), but a RECORD's struct is built lazily on first real use.
    // When a record field is read ONLY from inside that field's own
    // computed-property getter body (`property Getter: int => _pub;`, e.g.
    // via emitExportedPropertyGetters/ensurePropertyAccessorFn — #2474's own
    // fix widened that path to records — with no OTHER use of the record
    // earlier in the same module), THIS call can be the very first thing to
    // touch the record's layout at all, so the struct genuinely has not
    // been built yet. The sibling qualified-access path, `fieldAddr` (a few
    // hundred lines below in this file), already retries via
    // `classStruct(ms->parent)` before concluding a field is absent —
    // exactly the "imported/sibling path does a step this one skips" shape
    // (lane protocol) except the sibling here isn't cross-module, it's this
    // function's own qualified-access twin. Do not conclude "static field"
    // (and return null, silently collapsing the caller's whole expression to
    // a zero constant via ensurePropertyAccessorFn's own null-value
    // fallback) until AFTER the retry below — which needs the ancestor
    // generic-substitution setup installed first, exactly as fieldAddr does,
    // so it is deferred to occur once that setup below has run rather than
    // duplicated here.
    // §12 / §14 (#989): an INHERITED field declared on a generic base (ms->parent
    // has generic params) accessed via bare identifier inside a derived-class
    // method (implicit `this.field`). classStruct(ms->parent) with no active
    // genericSubst_ uses the T→i32 placeholder layout, causing a type mismatch
    // between the GEP element type and the field's concrete type (IR_FAIL for
    // string/bool return; SIGSEGV for class-pointer field; wrong value for int).
    // Install the ancestor generic substitution from the enclosing class (looked
    // up via currentFn_/methodFns_) before calling classStruct, then restore —
    // exactly mirroring the qualified fieldAddr path at LlvmBackend_Layout.cpp
    // around line 2720.
    auto savedSubst = genericSubst_;
    auto savedSemaTy = genericSubstSemaTy_;
    if (!ms->parent->genericParams.empty()) {
        // Recover the enclosing class. Two cases:
        //  (a) emitting a method's STANDALONE body (currentFn_ is the method's
        //      own compiled function): the enclosing class is the method's
        //      declaring class.
        //  (b) inlining a method body via emitInlineGenericMethod: currentFn_
        //      is the CALLER's function (e.g. main()), and currentSpecMethodSymbol_
        //      names the method being inlined. The semantically correct
        //      enclosing class is the INLINED method's class — `this` in the
        //      inlined body is an instance of that class, and the base-chain
        //      walk must START from it to find the right inherited generic
        //      param binding.
        //
        // fix #3347: prefer currentSpecMethodSymbol_->parent when it is set
        // (case b). The previous `currentSpecMethodSymbol_` was only a FALLBACK
        // to the currentFn_ lookup, which finds the CALLER's class — not the
        // inlined method's. For `new GD<Pt>(new Pt(7, 8))` against
        //   class GD<U> : GBase<U> { method V(): U => _v; }
        // the inlined V() body reads `_v` from GBase<T>; the active
        // genericSubst_ binds U→Pt but does not bind GBase's T, so the
        // computeAncestorGenericSubst walk must START from GD (the inlined
        // method's class) to derive T→Pt. Starting from main()'s C
        // (case b's caller) finds no path to GBase and returns false, leaving
        // the substitution empty — classStruct(GBase) returns the
        // T→i32 placeholder spec, the inlined GEP reads only the first 4 bytes
        // of `_v`, and `V()` returns X=7 with Y=garbage. Case (a) is unaffected:
        // currentSpecMethodSymbol_ is unset during standalone body emission.
        const sema::TypeSymbol* enclosingCls = nullptr;
        if (currentSpecMethodSymbol_ && currentSpecMethodSymbol_->parent)
            enclosingCls = currentSpecMethodSymbol_->parent;
        if (!enclosingCls && currentFn_) {
            for (const auto& [sym, fn] : methodFns_)
                if (fn == currentFn_ && sym->parent) {
                    enclosingCls = sym->parent;
                    break;
                }
        }
        if (enclosingCls && enclosingCls != ms->parent) {
            // Build a synthetic TypeRef for the enclosing class (concrete,
            // no generic args of its own) and walk the base chain to bind
            // ms->parent's generic params to their concrete types.
            sema::TypeRef synth;
            synth.kind = sema::TypeRef::Kind::Named;
            synth.declaration = const_cast<sema::TypeSymbol*>(enclosingCls);
            std::unordered_map<std::string_view, llvm::Type*> ancSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*> ancSema;
            if (computeAncestorGenericSubst(&synth, ms->parent, ancSubst,
                                            ancSema)) {
                genericSubst_ = std::move(ancSubst);
                genericSubstSemaTy_ = std::move(ancSema);
            }
        }
    }
    if (idxIt == fieldIndex_.end()) {
        classStruct(ms->parent);
        idxIt = fieldIndex_.find(ms);
        // fix #2474: still absent after the struct is actually built ->
        // genuinely a static field (or otherwise not instance storage), the
        // ORIGINAL "static field" conclusion — restore state and bail, same
        // as the pre-fix unconditional-null return did, just now correctly
        // deferred until the lazy build has had a chance to populate it.
        if (idxIt == fieldIndex_.end()) {
            genericSubst_ = std::move(savedSubst);
            genericSubstSemaTy_ = std::move(savedSemaTy);
            return nullptr;
        }
    }
    llvm::StructType* st = classStruct(ms->parent);
    elemTy = st->getElementType(idxIt->second);
    llvm::Value* addr = builder_.CreateStructGEP(st, currentThis_, idxIt->second);
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    return addr;
}


// §8.3 / §9.1: compare one field value of declared Bezel type `fieldTy`.
// Strings compare by content (§4.6) — the same content equality the `=`
// operator gives strings, and consistent with the content-based hashing in
// emitValueHashCode; two records with equal string content are equal even when
// the strings are distinct heap objects. Nested records recurse structurally
// (§9.1 transitivity). Primitives and enums compare by value; class references
// compare by identity (reference equality, §8.4). When `fieldTy` is null the
// comparison falls back to the LLVM value shape (string ptr → content, struct →
// recurse, float → FCmp, integer/ptr → ICmp).
llvm::Value* Emitter::emitValueEquality(llvm::Value* l, llvm::Value* r,
                                        const sema::TypeRef* fieldTy) {
    using K = sema::TypeRef::Kind;
    // per §8.3/§9.1: a NULLABLE REFERENCE field (string?, Object?, nullable
    // class/interface) lowers to a plain `ptr` whose null is a real null
    // pointer. Equality is: both-null → equal; exactly-one-null → not-equal;
    // neither-null → the type's normal comparison (content for string,
    // identity for refs). Guard the one-null case BEFORE any loadStringLen /
    // content compare so a null field can't be dereferenced (bug A).
    if (isNullableRefTy(fieldTy) && l->getType()->isPointerTy() &&
        r->getType()->isPointerTy()) {
        auto* nullP = llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* lNull = builder_.CreateICmpEQ(l, nullP, "eq.lnull");
        llvm::Value* rNull = builder_.CreateICmpEQ(r, nullP, "eq.rnull");
        llvm::Value* bothNull = builder_.CreateAnd(lNull, rNull, "eq.bothnull");
        llvm::Value* eitherNull = builder_.CreateOr(lNull, rNull, "eq.eithernull");
        auto* cmpBB  = llvm::BasicBlock::Create(llvm_, "eq.cmp", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "eq.end", currentFn_);
        auto* entryBB = builder_.GetInsertBlock();
        // If either is null, skip the content compare; result = bothNull.
        builder_.CreateCondBr(eitherNull, endBB, cmpBB);
        builder_.SetInsertPoint(cmpBB);
        bool isStr = fieldTy->kind == K::Primitive &&
                     fieldTy->primitive == sema::PrimitiveType::String;
        // Both non-null: string → content equality; ref → identity.
        llvm::Value* cmp = isStr ? emitStringEq(l, r)
                                 : builder_.CreateICmpEQ(l, r, "eq.refid");
        auto* cmpEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2, "eq.nullable");
        phi->addIncoming(bothNull, entryBB);
        phi->addIncoming(cmp, cmpEnd);
        return phi;
    }
    // per §8.3/§9.1 / issue #364: a NULLABLE VALUE field (int?, bool?, double?,
    // … the value primitives isNullablePrimTy recognizes, AND a nullable enum —
    // isNullableValueTy) lowers to a tagged struct {i8 hasValue; T value}
    // (nullableValueStructTy). The null-safe `=` of §8.4 is: both-null → equal;
    // exactly-one-null → not-equal; neither-null → the inner T's `=`. Drive it
    // off the hasValue tags so the `value` slot is only compared when both are
    // present (an absent value's slot is zero-filled but meaningless).
    if (isNullableValueTy(fieldTy) && l->getType()->isStructTy() &&
        r->getType()->isStructTy()) {
        llvm::Value* lHas = nullablePrimHasValue(l);   // i1
        llvm::Value* rHas = nullablePrimHasValue(r);
        llvm::Value* tagsEq = builder_.CreateICmpEQ(lHas, rHas, "eq.nvtag");
        llvm::Value* bothHave = builder_.CreateAnd(lHas, rHas, "eq.nvboth");
        // Inner T equality of the value slots — primitives/enums compare by
        // value (the fall-through ICmp/FCmp below, reached via a nullable-off
        // recursion so floats use FCmp; an enum's i32 value uses ICmp).
        sema::TypeRef innerTy = *fieldTy;
        innerTy.nullable = false;
        llvm::Value* valEq = emitValueEquality(
            nullablePrimValue(l), nullablePrimValue(r), &innerTy);
        // equal  iff  tags equal AND (both-null OR inner values equal).
        // both-null is (tags equal AND !bothHave); neither-null path needs
        // valEq. Combine: tagsEq AND (NOT bothHave OR valEq).
        llvm::Value* notBoth = builder_.CreateNot(bothHave, "eq.nvnotboth");
        llvm::Value* inner = builder_.CreateOr(notBoth, valEq, "eq.nvinner");
        return builder_.CreateAnd(tagsEq, inner, "eq.nullableval");
    }
    // per §4.4 / fix #663: a non-nullable decimal field — the payload of a
    // decimal? or a bare decimal record field — lowers to %bz.decimal
    // ({i32×4}). Bit-wise struct equality is WRONG: 3.0m and 3.00m have
    // different scale bits but are semantically equal. Route via bz_decimal_cmp.
    if (fieldTy && fieldTy->kind == K::Primitive &&
        fieldTy->primitive == sema::PrimitiveType::Decimal &&
        !fieldTy->nullable &&
        l->getType() == decimalStructType() && r->getType() == decimalStructType()) {
        llvm::AllocaInst* aSlot = entryAlloca(decimalStructType());
        llvm::AllocaInst* bSlot = entryAlloca(decimalStructType());
        builder_.CreateStore(l, aSlot);
        builder_.CreateStore(r, bSlot);
        auto* cmpFn = declareC("bz_decimal_cmp", i32(), {ptrTy(), ptrTy()});
        llvm::Value* cmp = builder_.CreateCall(cmpFn, {aSlot, bSlot}, "dec.eq.cmp");
        return builder_.CreateICmpEQ(cmp, llvm::ConstantInt::get(i32(), 0), "dec.eq");
    }
    // String field: content equality (§4.6), not pointer identity. per §8.3
    bool isString = fieldTy && fieldTy->kind == K::Primitive &&
                    fieldTy->primitive == sema::PrimitiveType::String;
    if (isString && l->getType()->isPointerTy() && r->getType()->isPointerTy())
        return emitStringEq(l, r);
    // Inline-array element (§15.1.2): an `[N]T` element value lowers to an LLVM
    // ArrayType; recurse element-by-element under the array's own element type
    // (so e.g. a nested `[M]string` slot content-compares its strings). per §8.3
    if (fieldTy && fieldTy->kind == K::InlineArray &&
        l->getType()->isArrayTy()) {
        if (auto* nestedArr = llvm::dyn_cast<llvm::ArrayType>(l->getType()))
            return emitInlineArrayEquality(l, r, nestedArr, fieldTy->arrayElement);
    }
    // Nested record (§9.1 transitivity): recurse with its own type so that
    // record's string fields are likewise compared by content.
    //
    // fix #4234 (§9.1, v0.298): route through a per-INSTANTIATION MEMOIZED
    // helper (a real `call`) instead of re-inlining `emitStructEquality`'s
    // body here — see recordToStringHelperFn's comment (LlvmBackend_Setup.cpp)
    // for why the key/load type must be the operand's OWN struct type, not
    // one re-derived from `fieldTy->declaration` alone. A shared nested
    // record type reached from `branch` sibling FIELD positions otherwise
    // re-expands its entire recursive comparison at EVERY occurrence —
    // O(branch^depth) LLVM IR instructions for a program that only declares
    // the types (measured: an unfixed depth=8/branch=3 diamond alone cost
    // 167s once the sibling ToString blowup was fixed, confirming this arm
    // reproduces the identical defect class one level of nesting later).
    // `l`/`r` share one type by construction (a well-typed record `=`/
    // `Equals` never compares across instantiations), so `l`'s type keys
    // and loads both operands.
    if (fieldTy && fieldTy->kind == K::Named && fieldTy->declaration &&
        fieldTy->declaration->kind == ast::TypeDeclKind::Record &&
        l->getType()->isStructTy()) {
        // fix #4251 (§9.1/§14.1/§4.6, v0.298): install THIS field's own
        // generic substitution before the memoized helper's body is (first)
        // built. Issue #462 installs `RecordSubstGuard` only at the
        // TOP-LEVEL `=`/`Equals` dispatch (LlvmBackend_Exprs.cpp's
        // `eqSubst`); a NON-generic outer record whose FIELD is a generic
        // record (`record SoloStr(BS: Box<string>)`) never gets any guard
        // for that field's own instantiation, so `recordEqualityHelperFn`'s
        // body (built via `emitStructEquality` on `Box`'s AST decl) ran
        // `recordMemberTypeResolved` on Box's `V: T` field under an EMPTY
        // `genericSubstSemaTy_` — it came back bare, unresolved `T`, which
        // the string-content-equality arm above never matches, so it fell
        // to the raw-shape fallback's `ICmpEQ` on the two `bz_string*`
        // pointers: REFERENCE equality, not §4.6 content equality (measured:
        // `SoloStr(Box("hi")) = SoloStr(Box("hi"))` was `False`). `fieldTy`
        // here already carries the CONCRETE `genericArgs` (`Box<string>`),
        // so a guard keyed on it binds `T` -> `string` for exactly this
        // (first) build. A no-op for a non-generic nested record (empty
        // `genericArgs`) or an already-concrete one. `RecordSubstGuard`'s
        // own ctor resolves each arg through the CURRENTLY active (outer)
        // substitution before overwriting it, so this also chains correctly
        // when the outer record is itself generic and this field's arg is
        // that outer type parameter.
        RecordSubstGuard fieldSubst(this, fieldTy);
        auto* recTy = llvm::cast<llvm::StructType>(l->getType());
        llvm::Function* helper =
            recordEqualityHelperFn(fieldTy->declaration, recTy);
        llvm::AllocaInst* lSpill = entryAlloca(recTy);
        llvm::AllocaInst* rSpill = entryAlloca(recTy);
        builder_.CreateStore(l, lSpill);
        builder_.CreateStore(r, rSpill);
        return builder_.CreateCall(helper->getFunctionType(), helper,
                                   {lSpill, rSpill});
    }
    // §9.2 v0.210 (#2083) / §35.7: a value-like choice component — the
    // declared type is a Named with declaration's kind == Choice and the
    // choice is auto-hashable — must compare by active-case then
    // per-case payload. The bit-level fallback below would compare *all*
    // payload slots regardless of the active case, which is unsound: the
    // inactive-case payload slots are `undef` in a freshly constructed
    // choice (different undefs per construction → different bits → false
    // equality), and §35.7's spec rule is exact on the active case. Recurse
    // into the dedicated handler that gates payload compare on tag equal.
    //
    // Borrow-bearing and owning choices are NOT eligible record
    // components (§9.2 admissible list, mirrored by the runtime check): a
    // record field of a borrow-bearing choice would compare class
    // references inconsistently, and an owning choice is move-only (cannot
    // be a record field, §9.7). For the rare case where the type checker
    // admitted an inadmissible choice through some gap, we fall through to
    // the bit-level ICmpEQ of the tag — at worst the user sees a wrong
    // equality; the sema upstream is the canonical enforcement.
    if (fieldTy && fieldTy->kind == K::Named && fieldTy->declaration &&
        fieldTy->declaration->kind == ast::TypeDeclKind::Choice &&
        l->getType()->isStructTy() && r->getType()->isStructTy())
        return emitChoiceEquality(l, r, fieldTy->declaration);
    // Fallback by LLVM shape (covers null fieldTy and primitives/enums/refs).
    // Nested structs recurse; floats use FCmp; everything else (integers,
    // and ptr fields when the declared type is unknown) compares by ICmp —
    // identical to the pre-fix behavior. A string field is only reliably
    // identified by its declared type, so when `fieldTy` is absent we do NOT
    // assume ptr-means-string (that would wrongly content-compare class
    // references). Both real call sites pass the record type, so this fallback
    // is purely defensive.
    if (auto* nested = llvm::dyn_cast<llvm::StructType>(l->getType()))
        return emitStructEquality(l, r, nested);
    // Inline-array shape with no declared element type (defensive): recurse
    // element-wise so an aggregate operand never reaches the scalar ICmp below
    // (which rejects array operands). per §15.1.2
    if (auto* arr = llvm::dyn_cast<llvm::ArrayType>(l->getType()))
        return emitInlineArrayEquality(l, r, arr);
    if (l->getType()->isFloatingPointTy()) {
        // §9.2 / §4.4 (fix #1165): a float/double field's `=` must use the
        // SAME NaN-aware reflexive equality the scalar `=` uses (§4.3.1:
        // "NaN = NaN is true"), not raw IEEE oeq. Mirror the scalar lowering in
        // LlvmBackend_Exprs.cpp (`oeq(l,r) OR (isNaN(l) AND isNaN(r))`) so two
        // records with equal NaN fields compare equal. isNaN(x) is `fcmp uno
        // x, x`. (+0.0/-0.0 stay equal: oeq is already true for them.)
        llvm::Value* ieee_eq  = builder_.CreateFCmpOEQ(l, r, "feq");
        llvm::Value* l_is_nan = builder_.CreateFCmpUNO(l, l, "lnan");
        llvm::Value* r_is_nan = builder_.CreateFCmpUNO(r, r, "rnan");
        llvm::Value* both_nan = builder_.CreateAnd(l_is_nan, r_is_nan, "bnan");
        return builder_.CreateOr(ieee_eq, both_nan, "feq_ref");
    }
    return builder_.CreateICmpEQ(l, r);
}

llvm::Value* Emitter::emitStructEquality(llvm::Value* l, llvm::Value* r,
                                llvm::StructType* st,
                                const sema::TypeSymbol* rec) {
    llvm::Value* acc = builder_.getInt1(true);
    // §9.1/§9.2: when the record type is known, drive comparison off each
    // storage member's declared Bezel type (the same order recordStruct lays
    // out, and the same collectRecordStorage / recordMemberType machinery the
    // synthesized GetHashCode uses) so string fields compare by content.
    if (rec) {
        std::vector<const sema::Symbol*> storage;
        collectRecordStorage(rec, storage);
        unsigned i = 0;
        for (const sema::Symbol* m : storage) {
            if (i >= st->getNumElements()) break;
            llvm::Value* li = builder_.CreateExtractValue(l, {i});
            llvm::Value* ri = builder_.CreateExtractValue(r, {i});
            // issue #462 / §14.1: resolve a generic record's `T`-typed field to
            // its concrete type under the active substitution so a string field
            // compares by CONTENT (not pointer bits) and a record field recurses.
            acc = builder_.CreateAnd(
                acc, emitValueEquality(li, ri, recordMemberTypeResolved(m)));
            ++i;
        }
        return acc;
    }
    // No record type available: compare by LLVM field shape (per-field
    // emitValueEquality with a null declared type — recurses into nested
    // structs, content-compares ptr fields). per §8.3
    for (unsigned i = 0; i < st->getNumElements(); ++i) {
        llvm::Value* li = builder_.CreateExtractValue(l, {i});
        llvm::Value* ri = builder_.CreateExtractValue(r, {i});
        acc = builder_.CreateAnd(acc, emitValueEquality(li, ri, nullptr));
    }
    return acc;
}

llvm::Value* Emitter::emitInlineArrayEquality(llvm::Value* l, llvm::Value* r,
                                llvm::ArrayType* at,
                                const sema::TypeRef* elemTy) {
    // §8.3 (equality table) / §15.1.2: "Two inline-array values of the same
    // type are equal iff their elements are pairwise equal under the element
    // type's equality. Inline arrays follow record-equality discipline:
    // structural equality grounded in element equality, with no special cases."
    // Parallels emitStructEquality: extract each slot and AND the element-wise
    // comparisons. The slot count is fixed at compile time (N is constant per
    // §15.1.2), so an unrolled compare matches the record-equality machinery.
    // Each element is compared via its declared Bezel type (emitValueEquality):
    // string slots compare by content (§4.6), record slots recurse via
    // emitStructEquality, nested inline arrays recurse, primitives/enums use
    // ICmp and floats use FCmp.
    llvm::Value* acc = builder_.getInt1(true);
    for (unsigned i = 0; i < at->getNumElements(); ++i) {
        llvm::Value* li = builder_.CreateExtractValue(l, {i});
        llvm::Value* ri = builder_.CreateExtractValue(r, {i});
        acc = builder_.CreateAnd(acc, emitValueEquality(li, ri, elemTy));
    }
    return acc;
}

// §35.7 (v0.210, #2083): choice equality. The choice struct is laid out as
// `{ i32 tag, case0_payload_struct, case1_payload_struct, ... }` (see
// choiceStruct). Two choices are equal iff (a) their tags match, and (b)
// the active case's payload fields compare pairwise by the case's payload
// declared types. Inactive-case payload slots are NOT compared — they
// contain `undef` for a freshly constructed value, and any tag-indifferent
// comparison would yield spurious inequality (the pre-fix bug that caused
// two equal-Red values to compare as unequal: tag matched, but the
// inactive case1 / case2 / case0's i8 placeholder versus itself came out
// as `undef` from the LLVM `false` constant); §35.7 is exact on "same
// active case and every corresponding payload compares equal", so the
// sound implementation here matches.
//
// REAL CONTROL FLOW, not a select chain (#2083 rework): the payload compare
// of case ci must execute ONLY when ci is the active case. Inactive-case
// payload slots hold `undef` (construction fills only tag + the active
// payload, LlvmBackend_Calls2.cpp), so a straight-line emit that runs every
// case's compare and merely SELECTs the result still *executes* the side
// effects — a string payload's emitStringEq walks an undef pointer, a
// record/nested-choice payload recurses into loads through undef — which is
// UB and an ASan crash, regardless of the select masking the result. The
// shape mirrors emitOwningChoiceActiveCaseDisposal (§35.4): switch on the
// tag, one basic block per payload-bearing case, PHI join. The switch
// default (payload-free cases and, defensively, an out-of-range tag) yields
// true — a payload-free case is equal to itself by tag alone.
llvm::Value* Emitter::emitChoiceEquality(llvm::Value* l, llvm::Value* r,
                                         const sema::TypeSymbol* choiceTy) {
    llvm::Value* lTag = builder_.CreateExtractValue(l, {0u}, "cheq.ltag");
    llvm::Value* rTag = builder_.CreateExtractValue(r, {0u}, "cheq.rtag");
    llvm::Value* tagsEq = builder_.CreateICmpEQ(lTag, rTag, "cheq.tag");
    auto* entryBB = builder_.GetInsertBlock();
    auto* swBB  = llvm::BasicBlock::Create(llvm_, "cheq.sw",  currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "cheq.end", currentFn_);
    builder_.CreateCondBr(tagsEq, swBB, endBB);

    // Join incomings: (pred block, i1 value). Different tags → false from
    // the entry block; the switch default → true (payload-free / defensive
    // out-of-range); each payload-bearing arm → its per-payload AND.
    std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>> incoming;
    incoming.emplace_back(entryBB, builder_.getInt1(false));

    builder_.SetInsertPoint(swBB);
    auto cases = choiceCasesOf(choiceTy);
    auto* sw = builder_.CreateSwitch(lTag, endBB,
                                     static_cast<unsigned>(cases.size()));
    incoming.emplace_back(swBB, builder_.getInt1(true));
    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        const sema::ChoiceCaseSymbol* cc = cases[ci];
        if (!cc || !cc->caseDecl || cc->caseDecl->payload.empty())
            continue;  // payload-free — arm collapses to the default (true).
        auto* armBB = llvm::BasicBlock::Create(llvm_, "cheq.arm", currentFn_);
        sw->addCase(builder_.getInt32(static_cast<int>(ci)), armBB);
        builder_.SetInsertPoint(armBB);
        // Active case ci: compare its payload fields pairwise. The field
        // index in the choice struct: the ci-th case's payload slot is
        // struct field 1+ci; its pi-th payload field is {1+ci, pi}. The
        // extracts and the per-field equality (which may itself emit
        // control flow — a string compare, a nested record/choice recurse)
        // are all emitted inside this arm, so they run ONLY under tag==ci.
        llvm::Value* caseEq = builder_.getInt1(true);
        for (std::size_t pi = 0; pi < cc->caseDecl->payload.size(); ++pi) {
            const ast::Param& pf = cc->caseDecl->payload[pi];
            llvm::Value* lf = builder_.CreateExtractValue(
                l, {static_cast<unsigned>(1 + ci),
                    static_cast<unsigned>(pi)}, "cheq.lf");
            llvm::Value* rf = builder_.CreateExtractValue(
                r, {static_cast<unsigned>(1 + ci),
                    static_cast<unsigned>(pi)}, "cheq.rf");
            sema::TypeRef pTy;  // resolved payload type for emitValueEquality
            if (pf.type)
                // §14.1 (#2047): resolve a bare-param payload (`T`) through the
                // active instantiation subst so a `string`/record payload of a
                // generic choice compares by its CONCRETE type (content), not the
                // i32-placeholder. A no-op for a concrete payload type.
                if (const sema::TypeRef* rt =
                        resolveTypeParamRef(ctx_.resolvedType(*pf.type)))
                    pTy = *rt;
            caseEq = builder_.CreateAnd(caseEq,
                                        emitValueEquality(lf, rf, &pTy));
        }
        // The per-field equality may have opened nested blocks — the arm's
        // terminator belongs on the CURRENT block, and that block is the
        // PHI's predecessor.
        auto* armEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        incoming.emplace_back(armEnd, caseEq);
    }
    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(i1(), static_cast<unsigned>(incoming.size()),
                                   "cheq");
    for (auto& [bb, v] : incoming) phi->addIncoming(v, bb);
    return phi;
}

// §35.7 (v0.210, #2083): choice hash. The choice struct's i32 tag is
// hashed first (h = 17*31 + tag), then the ACTIVE case's payload fields
// are folded in (h = h*31 + fieldHash). For a payload-free case the
// payload contributes nothing beyond the tag. Per §35.7 the discriminant
// is part of the hash (so two values in different cases never collide),
// and ONLY the active case's payload participates — equal values must
// hash equal even when their inactive (undef) slots differ.
//
// REAL CONTROL FLOW, not a select chain (#2083 rework): the per-case
// payload hash of case ci must EXECUTE only when ci is the active case.
// A straight-line emit ran every case's fold unconditionally — for a
// string-bearing choice in a payload-free active case, emitValueHashCode
// deterministically walked the string-hash loop through an undef pointer
// (there was no tag precondition at all), a guaranteed crash. Switch on
// the tag, one basic block per payload-bearing case, PHI join — the same
// shape as emitChoiceEquality / emitOwningChoiceActiveCaseDisposal. The
// switch default (payload-free cases and, defensively, an out-of-range
// tag) yields the tag-only hash.
llvm::Value* Emitter::emitChoiceHashCode(llvm::Value* val,
                                         const sema::TypeSymbol* choiceTy) {
    llvm::Value* tag = builder_.CreateExtractValue(val, {0u}, "ch.tag");
    // Seed: hash the tag with a non-zero starting point, so a payload-free
    // choice variant still has a meaningful hash (mirrors the record seed
    // in emitRecordHashCode).
    llvm::Value* tagMix = builder_.CreateAdd(
        builder_.CreateMul(builder_.getInt32(17), builder_.getInt32(31)),
        tag, "ch.tagmix");
    auto cases = choiceCasesOf(choiceTy);
    bool anyPayload = false;
    for (const sema::ChoiceCaseSymbol* cc : cases)
        if (cc && cc->caseDecl && !cc->caseDecl->payload.empty())
            anyPayload = true;
    if (!anyPayload) return tagMix;  // all cases payload-free — tag-only hash,
                                     // no control flow needed.
    auto* entryBB = builder_.GetInsertBlock();
    auto* endBB = llvm::BasicBlock::Create(llvm_, "ch.end", currentFn_);
    auto* sw = builder_.CreateSwitch(tag, endBB,
                                     static_cast<unsigned>(cases.size()));
    // Join incomings: default (payload-free / out-of-range) → tag-only hash;
    // each payload-bearing arm → its per-payload fold.
    std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>> incoming;
    incoming.emplace_back(entryBB, tagMix);
    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        const sema::ChoiceCaseSymbol* cc = cases[ci];
        if (!cc || !cc->caseDecl || cc->caseDecl->payload.empty())
            continue;  // payload-free — arm collapses to the default.
        auto* armBB = llvm::BasicBlock::Create(llvm_, "ch.arm", currentFn_);
        sw->addCase(builder_.getInt32(static_cast<int>(ci)), armBB);
        builder_.SetInsertPoint(armBB);
        llvm::Value* caseH = tagMix;  // base: tag folded in once.
        for (std::size_t pi = 0; pi < cc->caseDecl->payload.size(); ++pi) {
            const ast::Param& pf = cc->caseDecl->payload[pi];
            sema::TypeRef pTy;
            if (pf.type)
                // §14.1 (#2047): resolve a bare-param payload through the active
                // instantiation subst so a generic choice's `string`/record
                // payload hashes by its concrete type. A no-op for a concrete type.
                if (const sema::TypeRef* rt =
                        resolveTypeParamRef(ctx_.resolvedType(*pf.type)))
                    pTy = *rt;
            llvm::Value* pfVal = builder_.CreateExtractValue(
                val, {static_cast<unsigned>(1 + ci),
                      static_cast<unsigned>(pi)}, "ch.pf");
            // The payload hash (which may itself emit control flow — a
            // string content hash, a nested record/choice fold) is emitted
            // inside this arm, so it runs ONLY under tag==ci.
            llvm::Value* pfH = emitValueHashCode(pfVal, &pTy);
            caseH = builder_.CreateAdd(
                builder_.CreateMul(caseH, builder_.getInt32(31)), pfH);
        }
        auto* armEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        incoming.emplace_back(armEnd, caseH);
    }
    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(i32(),
                                   static_cast<unsigned>(incoming.size()),
                                   "ch.hash");
    for (auto& [bb, v] : incoming) phi->addIncoming(v, bb);
    return phi;
}


llvm::Value* Emitter::fieldAddr(const ast::MemberAccessExpr& ma,
                       llvm::Type*& elemTy, llvm::Value** outRecvPtr) {
    auto* ms = storageMemberOf(ctx_.exprSymbol(ma));
    // per §14.1/§14.2 (issue #505): when `ms` is null, sema did not resolve
    // the member access (e.g. `x.v` where `x` is an unannotated lambda param
    // of a generic method call — sema typed `x` as the bare T TypeRef so it
    // could not look up field `v`). If the TARGET identifier has a
    // lambdaParamTypeHint_ for a concrete class TypeRef (injected by the
    // pre-pass in lowerCall), resolve the member by name from that class.
    if (!ms && ma.target) {
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma.target.get())) {
            sema::Symbol* tSym = ctx_.exprSymbol(*id);
            auto hit = lambdaParamTypeHint_.find(tSym);
            if (hit != lambdaParamTypeHint_.end() && hit->second) {
                sema::TypeRef* hintTy = hit->second;
                if (hintTy->kind == sema::TypeRef::Kind::Named &&
                    hintTy->declaration) {
                    // Look up member by name on the hinted class/record.
                    for (sema::Symbol* m : hintTy->declaration->members) {
                        if (!m || m->name != ma.member) continue;
                        ms = storageMemberOf(m);
                        if (ms) break;
                    }
                }
            }
        }
    }
    // §9 / §12 / §14 (issue #590, follow-on to #585): when `ms` is null AND the
    // TARGET's static type is a bare GENERIC PARAMETER (`T` — a single-part Named
    // TypeRef with no declaration), sema could not resolve the nested member: a
    // field `_val: T` read as `_val.X` inside a generic method/ctor leaves `.X`
    // unresolved because `T` has no members at the declaration site. Inside a
    // MONOMORPHIZED body the active `genericSubstSemaTy_` binds `T` to its
    // concrete type for this instantiation (e.g. `T → Pt`); resolve the member
    // by name on that concrete type's declaration so `_val.X` GEPs the
    // specialized record/class layout (without this the read fell through to
    // `ret 0` — the standalone `Container<Pt>.GX()` and the inherited
    // `Wrapper<Pt>` paths all read 0).
    if (!ms && ma.target && !genericSubstSemaTy_.empty()) {
        if (sema::TypeRef* tt = ctx_.exprType(*ma.target)) {
            if (tt->kind == sema::TypeRef::Kind::Named && !tt->declaration &&
                tt->astNode && tt->astNode->name &&
                tt->astNode->name->parts.size() == 1) {
                auto cit = genericSubstSemaTy_.find(tt->headName());
                if (cit != genericSubstSemaTy_.end() && cit->second &&
                    cit->second->kind == sema::TypeRef::Kind::Named &&
                    cit->second->declaration) {
                    for (sema::Symbol* m : cit->second->declaration->members) {
                        if (!m || m->name != ma.member) continue;
                        ms = storageMemberOf(m);
                        if (ms) break;
                    }
                    // #2445 (§14.2, v0.236): the substitution-site check for a
                    // property/field READ on a bare type-parameter receiver.
                    // The active `genericSubstSemaTy_` binds the receiver's
                    // bare `T` to its concrete substituted type for THIS
                    // instantiation (e.g. `T → Pt`), and the loop above
                    // resolved the member by name on that substituted type's
                    // declaration. When the substituted type is a genuine
                    // record/class/choice/interface with a declaration but
                    // no member by that name exists, the substitution site
                    // IS where "no such member" is soundly decidable: the
                    // monomorphized body cannot be re-checked against any
                    // other substitution, and silently returning nullptr
                    // degrades the slot to a zero value via CG8's fall-
                    // through (#2547's generic "internal: codegen cannot
                    // read member...") — a silent wrong-value miscompile
                    // that the user has no way to trace back to the
                    // substituted type. Emit the spec-cited §14.2
                    // diagnostic here, naming the substituted type AND the
                    // type parameter so the user can either (a) add a
                    // `where T: <constraint>` that provides the member, or
                    // (b) correct the member name. The wording mirrors
                    // sema's #1078 reject (ExprTypecheck_Calls.cpp ~3095)
                    // so the two diagnostics are recognizably the same
                    // family. NOT fired for the substitution's own
                    // `declaration==nullptr` shape (e.g. a builtin
                    // declaration-less Named type like Array<T>, which has
                    // no member surface to look up) — that case is
                    // already covered by the CG8 fall-through diagnostic
                    // for the concrete builtin-family path. Likewise NOT
                    // fired for a `ma.member` that names a non-storage
                    // member (a method, event, property-getter without a
                    // backing field); such names are NOT in
                    // `declaration->members` by name, but a method-call
                    // form is sema-rejected upstream (#1078) and a
                    // property-getter read is handled by the
                    // property-read sibling site, not fieldAddr.
                    if (!ms && cit->second->declaration) {
                        std::string_view subName =
                            cit->second->declaration->name;
                        ctx_.diagnostics().report(
                            Severity::Error, ma.span.begin,
                            std::string("no member `") +
                                std::string(ma.member) +
                                "` on substituted type `" +
                                std::string(subName) +
                                "` for type parameter `" +
                                std::string(tt->headName()) +
                                "`; the member is not part of `" +
                                std::string(subName) +
                                "`'s documented surface, or add a `where " +
                                std::string(tt->headName()) +
                                ": <constraint>` that provides this "
                                "member (\xC2\xA7""14.2, #2445)");
                        return nullptr;
                    }
                }
            }
        }
    }
    // #2445 (§14.2, v0.236): RESIDUAL DECLINED — a property/field READ on a
    // bare type-parameter receiver when `genericSubstSemaTy_` is empty (the
    // generic method body is being validated independent of any
    // instantiation, or no caller in this compilation unit ever instantiates
    // it). Sema has no substitution information to consult, and codegen has
    // none either. The "is this member soundly decidable here?" question is
    // UNANSWERABLE at this site — the same architectural reason #1078's
    // sema-level allowlist was structurally unsound (Bezel generics are
    // monomorphized; sema runs ONCE, at the generic method's declaration
    // site, with no visibility into the unenumerable, open-ended set of
    // call-site type arguments). The original issue (#2445) explicitly
    // flagged this as a "residual, unresolved question — possibly a
    // 'worth a spec/rationale check' question per CLAUDE.md's
    // decline-with-reasons discipline, since this may be an intentional
    // consequence of the monomorphization design (comparable to C++
    // template bodies not being validated for dependent names until
    // instantiated)". A diagnostic here would also break legitimate
    // uses — confirmed live: a `Container<Pt>.GetX()` reading
    // `_val.X` on a bare-T field is emitted TWICE (once monomorphized
    // with `T → Pt` for the call site, and once for the dead
    // template-body IR-verification pass without substitution); the
    // template-body pass would trip this check on the legitimate
    // member read even though the monomorphized body resolves it
    // correctly. Per CLAUDE.md "decline-with-reasons, never silently"
    // discipline: the residual case is filed as a follow-up issue for
    // an owner/spec decision (whether the dead template body needs a
    // diagnostic at all, and if so what wording distinguishes it from
    // the substitution-site error). Today the existing CG8 fall-through
    // generic #2547 message continues to fire on this path for the
    // bogus-member case, so the silent-fallthrough miscompile is no
    // worse than the pre-#2445 state.
    if (!ms || !ms->parent || !ma.target) return nullptr;
    auto idxIt = fieldIndex_.find(ms);
    // CG-generic: for generic class field access (e.g. bd.value where
    // bd: Box<double>), set up the substitution from the target's exprType
    // so classStruct uses the correct concrete field types.
    auto savedSubst = genericSubst_;
    // §14.1 (#786): also save/build the SEMA-type substitution. The LLVM-type
    // map (genericSubst_) alone is insufficient for a nullable VALUE field
    // `V: T?` — its layout (`{i8, value}`) is driven by genericSubstSemaTy_ in
    // lowerTypeRef's nullable bare-param branch, which falls to the `{i8, i32}`
    // placeholder when that map is empty. So a `Box<bool>` field force-unwrap
    // read `b.V!` GEP'd an i32 value and `br i32 %nv.val` failed IR verification
    // (i32 is not i1). Mirror RecordSubstGuard/#766/#768: install both maps.
    auto savedSemaTy = genericSubstSemaTy_;
    if (sema::TypeRef* targetTy = ctx_.exprType(*ma.target)) {
        if (targetTy->kind == sema::TypeRef::Kind::Named &&
            targetTy->declaration == ms->parent &&
            !targetTy->genericArgs.empty()) {
            // fix #3143 (§14.1 / §2.5, v0.254): ONE branch, reading the
            // parameter names from whichever source carries them. This used to
            // be an AST arm plus #428's separate imported arm, and they had
            // DRIFTED — only the AST arm ran the #828/#2014 `hasUnresolved`
            // recovery through `localGenericSubst_`, so an imported generic
            // receiver reached through a bare-parameter argument skipped it.
            const std::vector<std::string_view> gpNames =
                sema::genericParamNames(ms->parent);
            if (!gpNames.empty() &&
                gpNames.size() == targetTy->genericArgs.size()) {
                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*>
                    newSema;
                for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                    newSubst[gpNames[gi]] =
                        lowerTypeRef(targetTy->genericArgs[gi]);
                    // §14.1 (#786): resolve a bare-param arg through the outer
                    // subst (built BEFORE genericSubstSemaTy_ is replaced).
                    newSema[gpNames[gi]] =
                        resolveTypeParamRef(targetTy->genericArgs[gi]);
                }
                // If any generic ARG is genuinely an unresolved type parameter,
                // try looking up the local's stored concrete substitution.
                // #828: detect this from the SEMA arg (a bare single-part Named
                // ref with no declaration — an actual type parameter), NOT from
                // the lowered LLVM type being i32. A concrete `int` arg also
                // lowers to i32, so the old `ty == i32()` test misfired on a
                // nested `Box<Box<int>>` / `Box<int>`: it read the inner instance's
                // legitimate `T = int` as "unresolved" and overrode it with the
                // OUTER local's substitution (`T → ptr`), loading the inner field
                // at ptr width (ret-ptr-vs-i32 IR_FAIL / a cast-assertion abort).
                bool hasUnresolved = false;
                for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                    const sema::TypeRef* arg = targetTy->genericArgs[gi];
                    if (arg && arg->kind == sema::TypeRef::Kind::Named &&
                        !arg->declaration && arg->astNode &&
                        arg->astNode->name &&
                        arg->astNode->name->parts.size() == 1) {
                        // #2014 / §14: a bare-param arg (`Box<U>`) is only
                        // GENUINELY unresolved when the ACTIVE substitution does
                        // not already bind it. In a MONOMORPHIZED enclosing body
                        // (e.g. `Run<U>` specialized to U=long) the active subst
                        // resolves `U` to its concrete type, so `newSubst[param]`
                        // above is ALREADY the correct concrete layout type
                        // (lowerTypeRef(arg) honored genericSubst_). Overriding it
                        // with localGenericSubst_ is wrong here: that map is keyed
                        // on the ENCLOSING method's param name (`U`), NOT this
                        // record's param (`T`), so classStruct(ms->parent) cannot
                        // find `T`, falls back to the i32 template layout, and GEPs
                        // the wrong specialized-struct offset — a garbage owned-
                        // field read → SEGV (`move box.f!`, #2014). Only when the
                        // param stays a bare param after resolution (the #828 case:
                        // no active binding, so we need the local's stored subst)
                        // do we fall through to the localGenericSubst_ lookup.
                        const sema::TypeRef* argR = resolveTypeParamRef(arg);
                        if (argR && argR->kind == sema::TypeRef::Kind::Named &&
                            !argR->declaration) {
                            hasUnresolved = true;
                            break;
                        }
                    }
                }
                if (hasUnresolved) {
                    // Chase through PostfixBangExpr/MemberAccessExpr to
                    // find the root local variable (e.g. root._left!._val
                    // peels !._left to reach `root`).
                    const ast::Expr* cur = ma.target.get();
                    const ast::Node* decl = nullptr;
                    while (cur && !decl) {
                        sema::Symbol* sym = ctx_.exprSymbol(*cur);
                        if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym))
                            decl = loc->decl;
                        if (!decl) {
                            if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(cur))
                                cur = bg->operand.get();
                            else if (auto* sub = dynamic_cast<const ast::MemberAccessExpr*>(cur))
                                cur = sub->target.get();
                            else
                                break;
                        }
                    }
                    if (decl) {
                        auto lit = localGenericSubst_.find(decl);
                        if (lit != localGenericSubst_.end())
                            newSubst = lit->second;
                    }
                }
                // #428 / §14.1 / §2.5: this covers the IMPORTED generic type
                // too — no ast::TypeDecl, but TypeSymbol::genericParams has the
                // param names, so recordStruct/classStruct still produce the
                // correct specialized struct for field-index mapping.
                genericSubst_ = std::move(newSubst);
                genericSubstSemaTy_ = std::move(newSema);
            }
        } else if (targetTy->kind == sema::TypeRef::Kind::Named &&
                   targetTy->declaration &&
                   targetTy->declaration != ms->parent &&
                   !ms->parent->genericParams.empty()) {
            // §12 / §14 (issue #585): direct access of an INHERITED field
            // declared on a generic BASE (`ms->parent`) through a derived
            // instance (`targetTy` == Wrapper<Pt>). The branch above requires
            // targetTy->declaration == ms->parent, so it does not fire — without
            // a substitution classStruct(ms->parent) uses the T→i32 template
            // layout and the GEP/field type are wrong for a non-int base field
            // (read 0 / garbage). Bind the base's generic params to the concrete
            // arguments the derived class supplied (walking the base chain).
            //
            // fix #2840 (§12 / §14.1 / §2.5, v0.240): this branch used to
            // ALSO require `!targetTy->genericArgs.empty()` — i.e. the
            // receiver had to be a generic instantiation of its own. A
            // NON-generic derived class (`class D : Base<long>`) has no
            // generic args, so `d.v` / `d.tag` skipped the substitution
            // entirely and GEP'd `classStruct(Base)` under the EMPTY subst —
            // the T→i32 template layout — while `new D()` allocated the
            // correctly specialized derived struct. Writing `d.v` then stored
            // 8 bytes at the template's 4-byte-slot offset and silently
            // clobbered the following field (measured: `tag` read back 28,
            // the high half of the stored `long`); at `T = string` the same
            // mismatch produced an unlocated IR-verifier abort. The bare-
            // identifier twin of this access — `implicitFieldAddr`, fixed for
            // exactly this shape by #989 — already builds a synthetic
            // no-generic-args receiver TypeRef and calls
            // computeAncestorGenericSubst; this is the qualified-access path
            // doing the step its own sibling does. computeAncestorGenericSubst
            // seeds `curArgs` from `recvTy->genericArgs` against the
            // receiver's own param names, so an EMPTY-args non-generic
            // receiver is the 0 == 0 case and walks the chain correctly; it
            // returns false (leaving the substitution untouched) whenever the
            // chain cannot be resolved, so widening the guard cannot install a
            // wrong substitution.
            std::unordered_map<std::string_view, llvm::Type*> ancSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*> ancSema;
            if (computeAncestorGenericSubst(targetTy, ms->parent, ancSubst,
                                            ancSema)) {
                genericSubst_ = std::move(ancSubst);
                // §14.1 (#786): install the parallel sema-type map so an
                // inherited generic-base nullable field also resolves.
                genericSubstSemaTy_ = std::move(ancSema);
            }
        }
    }
    if (idxIt == fieldIndex_.end()) {
        // Re-run classStruct to populate fieldIndex_ under the substitution.
        classStruct(ms->parent);
        idxIt = fieldIndex_.find(ms);
    }
    if (idxIt == fieldIndex_.end()) {
        // §22.12 / #213 / #747: a STATIC field (or static auto-property) has no
        // slot in the instance struct, so its FieldSymbol is absent from
        // fieldIndex_ even after classStruct() repopulates it. Return nullptr so
        // the caller falls through to staticFieldGlobal(), which reads/writes the
        // program-lifetime backing global — exactly the path a qualified
        // `Type._x` access already takes (the read in LlvmBackend_Exprs.cpp and
        // the write in lvalueAddr both have that fallback). Without this guard
        // the `idxIt->second` dereferences below run off end() and SIGSEGV the
        // compiler on an instance-receiver static access (e.g. `c._x` where
        // `_x: int` is `public static`).
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        return nullptr;
    }
    llvm::StructType* st = classStruct(ms->parent);
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    // CG29: a record is a first-class struct value, not a pointer. To take
    // the address of one of its fields, GEP the record's storage:
    //  - an addressable target (a record local/parameter) → GEP its slot
    //    directly, the natural value-type access;
    //  - an already-pointer target (a `this` pointer inside a record
    //    method) → GEP it as-is;
    //  - a temporary record value (e.g. a freshly returned record) → spill
    //    it to a stack slot and GEP that.
    if (ms->parent->kind == ast::TypeDeclKind::Record) {
        // §7.2 / §7.4 / issue #416: when the target is a NULLABLE record
        // (`b: Box?`, member-accessed after `if b <> null`), its storage is the
        // tagged {i8 hasValue; <record payload>} struct, not the bare record
        // struct. Sema permits this access (the §7.4 narrowing keeps the
        // declared type, which is still `Box?` at codegen); the payload lives at
        // tag-struct index 1, so GEP into the payload first and then the field.
        // A failed runtime null (hasValue=0) is the caller's `!`/narrowing
        // responsibility — fieldAddr just locates the field address.
        bool nullableRecordTarget = false;
        if (sema::TypeRef* tt = ctx_.exprType(*ma.target))
            nullableRecordTarget = isNullableRecordTy(tt);
        // CG29: prefer lvalue address so writes go to the original
        // memory (e.g. grid[2].Cells[x] := v must update grid[2],
        // not a stack copy). Fall back to lowerExpr for temporaries
        // (e.g. a freshly returned record value).
        llvm::Value* base = nullptr;
        llvm::Type* baseTy = nullptr;
        // The struct the base pointer points at: the bare record struct, or the
        // tagged nullable-record struct when the target is a `record?`.
        llvm::StructType* baseStruct =
            nullableRecordTarget
                ? llvm::StructType::get(llvm_,
                      {nullablePrimHasValTy(), st})
                : st;
        // fix #3090 (§12.7 / §8.6, v0.252): `o.X` where `o: Object` is
        // §12.7-narrowed to this record reads the BOXED record, whose storage is
        // the box payload two words into the wrapper. Checked ahead of the
        // localSlot arm, which short-circuits lvalueAddr (where the sibling hook
        // lives) and would otherwise StructGEP the `Object` SLOT — reading the
        // box pointer's own bytes as the record's first fields (measured: `o.X`
        // on a `Pt(7, 9)` printed 707433248, and the two-field arithmetic raised
        // OverflowException).
        llvm::Type* narrowedTy = nullptr;
        if (llvm::Value* np = narrowedBoxPayloadAddr(*ma.target, narrowedTy)) {
            base = np;
        } else if (llvm::AllocaInst* slot = localSlot(*ma.target)) {
            // §27.5: a captured record local lives in its shared cell.
            llvm::Type* ety = nullptr;
            base = boxedStorage(slot, ety);
        } else {
            base = lvalueAddr(*ma.target, baseTy);
            if (!base) {
                llvm::Value* obj = lowerExpr(*ma.target);
                if (!obj) return nullptr;
                if (obj->getType()->isPointerTy()) {
                    base = obj;
                } else {
                    llvm::Value* tmp = entryAlloca(obj->getType());
                    builder_.CreateStore(obj, tmp);
                    base = tmp;
                }
            }
        }
        if (nullableRecordTarget) {
            // Step into the payload (tag-struct index 1) to reach the bare
            // record struct, then GEP the field on it below.
            base = builder_.CreateStructGEP(baseStruct, base, 1, "nr.payload");
        }
        elemTy = st->getElementType(idxIt->second);
        return builder_.CreateStructGEP(st, base, idxIt->second);
    }
    llvm::Value* obj = lowerExpr(*ma.target);
    if (!obj) return nullptr;
    // issue #486 / §14.2: guard against emitting a StructGEP from a
    // non-pointer receiver.  This arises when the unspecialized body of a
    // generic method (e.g. `GetName<T>(x: T) where T: IHasName`) is lowered
    // with T's LLVM fallback type `i32`.  In that body `x` lowers to i32;
    // a StructGEP on i32 is malformed IR ("GEP base pointer is not a vector").
    // lowerVirtualPropertyGet already returned nullptr for the same receiver
    // (its !isPointerTy guard at line 1657), so we reach here only for
    // interface/abstract auto-properties that also live in fieldIndex_
    // (bodyKind==Auto for both interface and abstract class declarations).
    // Return nullptr here so the unspecialized stub emits `ret i32 0` — the
    // same safe no-op as emitMethodCall's #484 guard.  The specialized
    // monomorphized body (T = Person, T = Square, …) has a real ptr receiver
    // and is dispatched correctly by lowerVirtualPropertyGet before reaching
    // this path.
    if (!obj->getType()->isPointerTy()) return nullptr;
    // #1039: expose the lowered class-instance receiver pointer so a value-read
    // caller can dispose a fresh `new <class>()` temporary after extracting a
    // value result (otherwise the temp — owned by nothing — leaks).
    if (outRecvPtr) *outRecvPtr = obj;
    // §28.2 v0.135 / issue #621: checked disposal mode guard for stored-field
    // access through a class receiver. Only class instances carry a per-object
    // disposed flag (records are value types with no disposal lifetime).
    //
    // Skip when the target is `this` or `base`: inside an instance method the
    // method-entry guard (issue #601) already covers all `this`-qualified
    // accesses, so adding a second check here is redundant and would double-
    // raise on the FIRST field access in a disposed method (which is already
    // caught by the entry guard). This is a deliberate choice documented here.
    //
    // This guard fires for both READS (lowerExpr's CG8 path calls fieldAddr to
    // get the address, then loads) and WRITES (lowerStmt → lvalueAddr →
    // fieldAddr), so a single insertion covers both.
    if (checkedDisposal_ && ms->parent->kind == ast::TypeDeclKind::Class) {
        bool isThis = false;
        if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                ma.target.get()))
            isThis = (tid->name == "this" || tid->name == "base");
        if (!isThis) {
            unsigned fi = disposedFlagIndex(ms->parent);
            llvm::Value* flagAddr =
                builder_.CreateStructGEP(st, obj, fi, "cd.flag");
            llvm::Value* disposed = builder_.CreateICmpNE(
                builder_.CreateLoad(disposedFlagTy(), flagAddr),
                llvm::ConstantInt::get(disposedFlagTy(), 0), "cd.disposed");
            emitGuardThrow(disposed, "DisposedAccessException");
        }
    }
    elemTy = st->getElementType(idxIt->second);
    return builder_.CreateStructGEP(st, obj, idxIt->second);
}


}  // namespace bezel::codegen
