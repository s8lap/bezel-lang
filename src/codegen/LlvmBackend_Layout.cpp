#include "codegen/LlvmBackend_impl.hpp"

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
    auto unsignedExpr = [&](const ast::Expr* e) {
        if (!e) return false;
        auto* rt = ctx_.exprType(*e);
        return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
               isUnsignedPrim(rt->primitive);
    };
    return unsignedExpr(bin.left.get()) && unsignedExpr(bin.right.get());
}


bool Emitter::isUnsignedTargetExpr(const ast::Expr& e) {
    auto* rt = ctx_.exprType(e);
    return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
           isUnsignedPrim(rt->primitive);
}


llvm::Value* Emitter::lvalueAddr(const ast::Expr& target, llvm::Type*& elemTy) {
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
        // #720 / §22.13: a static-field lvalue (write or compound assign) is a
        // "first use" — lazily initialize the owning type BEFORE the access, so a
        // write-before-read is not later clobbered by the lazy initializer.
        if (auto* sym = ctx_.exprSymbol(target))
            emitEnsureStaticInit(memberOwner(sym));
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


llvm::Value* Emitter::emitCompoundOp(const std::string& op, llvm::Value* l,
                            llvm::Value* r, bool fp, bool uns) {
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
    // #1043: an ARITHMETIC compound assignment on a SUB-i32 integer LHS
    // (`byte`/`sbyte`/`short`/`ushort`) is `b = subInt(b OP v)` with CHECKED
    // narrowing (§6.1). #1042 truncated the RHS to the LHS width and ran the op
    // there, which was correct for an in-range RHS but WRONG for an out-of-range
    // one: `byte 5 += 300` truncated 300→44 and yielded 49 instead of raising
    // OverflowException. Compute at the promoted width (i32) — reusing the i32
    // checked-arith via recursion — then narrow-CHECK the result back to the LHS
    // type so any out-of-range sum/difference/product/quotient throws. Bitwise
    // and shift ops stay on the LHS-width path below: they cannot overflow the
    // LHS width (`&`/`|`/`^` keep operand bits; `<<`/`>>` mask the count, #1041).
    if (l->getType()->isIntegerTy() && !l->getType()->isIntegerTy(1) &&
        l->getType()->getIntegerBitWidth() < 32 &&
        (op == "+" || op == "-" || op == "*" || op == "/" || op == "mod")) {
        llvm::Type* dstTy = l->getType();
        auto toI32 = [&](llvm::Value* v) -> llvm::Value* {
            if (v->getType()->isIntegerTy(32)) return v;
            return uns ? builder_.CreateZExt(v, i32())
                       : builder_.CreateSExt(v, i32());
        };
        llvm::Value* wide = emitCompoundOp(op, toI32(l), toI32(r),
                                           /*fp=*/false, uns);
        if (!wide) return nullptr;
        // §5.2 checked narrowing: trunc, extend back with the result's
        // signedness, compare to the i32 value — a mismatch means out of range.
        llvm::Value* trunc = builder_.CreateTrunc(wide, dstTy);
        llvm::Value* back = uns ? builder_.CreateZExt(trunc, i32())
                                : builder_.CreateSExt(trunc, i32());
        emitGuardThrow(builder_.CreateICmpNE(back, wide), "OverflowException");
        return trunc;
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
        // select 0 on that path. (`mod=` is not valid syntax per §17.2, so the
        // `mod` arm is currently unreachable, but kept correct for parallelism.)
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
    // §6.5 (#1041): the shift count is reduced modulo the operand's bit width —
    // exactly as the non-compound `<<`/`>>` path does (LlvmBackend_Exprs.cpp).
    // LLVM shl/lshr/ashr with a count >= the bit width produces poison, so
    // without this mask `x <<= 40` (i32) emitted `shl x, 40` and yielded a wrong
    // value (the non-compound `x << 40` correctly gave `x << 8`). Mask to
    // [0, bitwidth-1] before the shift.
    if (op == "<<" || op == ">>") {
        unsigned bits = l->getType()->getIntegerBitWidth();
        llvm::Value* cnt = builder_.CreateAnd(
            r, llvm::ConstantInt::get(r->getType(), bits - 1), "shcnt");
        if (op == "<<") return builder_.CreateShl(l, cnt);
        return uns ? builder_.CreateLShr(l, cnt) : builder_.CreateAShr(l, cnt);
    }
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
        !isBuiltinTypeRefName(rt->astNode->name->parts[0])) {
        if (!genericSubstSemaTy_.empty()) {
            sema::TypeRef nv;
            if (nullableTypeParamConcrete(rt, nv)) {
                if (isNullableValueTy(&nv)) return nullableValueStructTy(&nv);
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
            // which clobbered the heap-collection (Array<T>/OwnedArray<T>/…),
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
                auto sit = genericSubst_.find(rt->astNode->name->parts[0]);
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
                auto fn = rt->astNode->name->parts[0];
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
                auto n0 = rt->astNode->name->parts[0];
                if (n0 == "DateTime" || n0 == "TimeSpan") return i64();
            }
            if (rt->declaration && !rt->declaration->decl &&
                (rt->declaration->name == "DateTime" ||
                 rt->declaration->name == "TimeSpan")) {
                return i64();
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
                rt->astNode->name->parts[0] == "Guid") {
                return guidStructType();
            }
            // §4.15 (#549) / §4.14 (#779): ArraySpan/ReadOnlyArraySpan (borrowing)
            // and Span/ReadOnlySpan (owning) are value structs
            // { ptr source, i32 start, i32 length } — no heap pointer.
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty() &&
                (rt->astNode->name->parts[0] == "ArraySpan" ||
                 rt->astNode->name->parts[0] == "ReadOnlyArraySpan" ||
                 rt->astNode->name->parts[0] == "Span" ||
                 rt->astNode->name->parts[0] == "ReadOnlySpan" ||
                 // §3.3 (#883): ReadOnlyStringSpan shares the {ptr,i32,i32} shape
                 // (field 0 is a string pointer, not an array pointer).
                 rt->astNode->name->parts[0] == "ReadOnlyStringSpan")) {
                return arraySpanStructType();
            }
            // §2.15 / issue #687: JsonOptions is a compiler-known value record.
            // { i1 Indented, i32 IndentWidth, i1 AllowTrailingCommas,
            //   i1 IgnoreUnknownFields, i32 MaxDepth }
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty() &&
                rt->astNode->name->parts[0] == "JsonOptions") {
                return jsonOptionsStructType();
            }
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty()) {
                auto n = rt->astNode->name->parts[0];
                if (n == "Array" || n == "OwnedArray" ||
                    // §4.14: OwnedBuffer<T> is heap-allocated (bz_array_t*);
                    // without this it fell through to i32 default → IR mismatch.
                    n == "OwnedBuffer" ||
                    n == "Dictionary" || n == "OwnedDictionary" ||
                    n == "Stack" || n == "OwnedStack" ||
                    n == "Queue" || n == "OwnedQueue" ||
                    n == "Set" || n == "OwnedSet" ||
                    n == "LinkedList" || n == "OwnedLinkedList" ||
                    // §4.6: a LinkedListNode<T> variable holds a pointer to the
                    // bz_llist_node_t; it is the node handle / borrow from the list.
                    n == "LinkedListNode" ||
                    n == "StringBuilder" || n == "Random" ||
                    // §29: iteration interfaces are heap-allocated objects.
                    n == "IEnumerable" || n == "IEnumerator" ||
                    n == "OwnedEnumerable" || n == "OwnedEnumerator" ||
                    n == "IAsyncEnumerable" || n == "IAsyncEnumerator" ||
                    n == "OwnedAsyncEnumerable" || n == "OwnedAsyncEnumerator" ||
                    n == "IReadOnlyCollection" || n == "IReadOnlyList" ||
                    n == "ICollection" || n == "IList" ||
                    n == "IOwnedCollection" || n == "IOwnedList" ||
                    // §30: Task types are also heap-allocated.
                    n == "Task" || n == "OwnedTask" ||
                    // per §2.11 / §24.4: a Subscription is a runtime-managed
                    // opaque handle (bz_subscription_t*), returned by
                    // `<event>.Subscribe(...)` / CancellationToken.Register and
                    // disposed via bz_subscription_dispose. Without this it fell
                    // through to the i32 default, truncating the pointer when a
                    // Subscription was a method return / parameter / field — so
                    // its later `.Dispose()` saw a non-pointer and was skipped.
                    n == "Subscription" ||
                    // §6.1 / §30.8: CancellationToken is a builtin record whose
                    // value representation is a single borrow-pointer to the
                    // cancellation state cell (bz_cancel_state_t*).  It lowers
                    // to a pointer word (ptr) rather than an i32 so the borrowed
                    // state pointer is stored at full width.
                    // CancellationTokenSource is an owned class type — also a ptr.
                    n == "CancellationToken" ||
                    n == "CancellationTokenSource" ||
                    // §28.13 / stdlib §2.16 (#620): Shared<T> and Weak<T> are
                    // compiler-known heap-pointer types (no user TypeDecl).
                    // Without these entries lowerTypeRef fell through to the i32
                    // default → bz_shared_get/bz_weak_release called with i32
                    // instead of ptr (IR verification failure); Shared<T>? fell
                    // into the bare type-param nullable branch (returns {i8,i32}
                    // instead of ptr) → checkGEPType LLVM-assert crash.
                    n == "Shared" || n == "Weak" ||
                    // §30.14/§30.15 (#600 phase c): Channel<T> and ChannelSender<T>
                    // are compiler-known heap-pointer builtins (bz_channel_t* and
                    // bz_channel_sender_t* respectively). Both lower to ptrTy().
                    n == "Channel" || n == "ChannelSender" ||
                    // stdlib §5 (#651): IGrouping<TKey, T> is a compiler-known
                    // pointer (bz_grouping_t*) produced by GroupBy(...).ToArray().
                    n == "IGrouping" ||
                    // §24.3 (#747): IEvent<T>/IAsyncEvent<T> value = the bare
                    // bz_event_source_t* (a borrow of the declaring instance).
                    n == "IEvent" || n == "IAsyncEvent" ||
                    // §6.2 / #647: Mutex (bz_mutex_t*) and Lock (bz_lock_t*) are
                    // compiler-known heap-pointer builtins. Without these entries
                    // lowerTypeRef fell through to the i32 default →
                    // `await mutex.AcquireAsync()` returned type Lock was lowered
                    // to i32, making CreateLoad(i32, resPtr) load 4 bytes from an
                    // 8-byte result buffer; the loaded i32 was then used as a
                    // pointer by using-var disposal → segfault / IR mismatch.
                    n == "Mutex" || n == "Lock" ||
                    // §6.3 / #647: Semaphore (bz_semaphore_t*) and SemaphoreToken
                    // (bz_semaphore_token_t*) — same fix applied proactively.
                    n == "Semaphore" || n == "SemaphoreToken" ||
                    // §6.4 / #647: Atomic<T> and AtomicInteger<T> are compiler-known
                    // heap-pointer builtins (bz_atomic_t*).  Same ptrTy treatment as
                    // Mutex/Semaphore so lowerTypeRef returns ptrTy() not i32.
                    n == "Atomic" || n == "AtomicInteger" ||
                    // issue #545: Buffer<T> is a compiler-known heap-pointer builtin
                    // (bz_buffer_hdr_t* with inline payload).  Without this entry
                    // lowerTypeRef would fall through to the i32 default → GEP /
                    // load type mismatches at every Buffer<T> use site.
                    n == "Buffer") {
                    return ptrTy();
                }
            }
            // §6.1 / §30.8: a CancellationToken-typed variable produced by
            // cts.Token has a non-null builtinClassSym declaration (used to
            // carry the type name through sema); recognise it by declaration name
            // so lowerTypeRef returns ptrTy() for it.
            if (rt->declaration && !rt->declaration->decl &&
                (rt->declaration->name == "CancellationToken" ||
                 rt->declaration->name == "CancellationTokenSource")) {
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
                if (n == "Guid")  return guidStructType();
                if (n == "Range") return rangeStructType();
                // §4.15 (#549) / §4.14 (#779) / §3.3 (#883): span value structs.
                if (n == "ArraySpan" || n == "ReadOnlyArraySpan" ||
                    n == "Span" || n == "ReadOnlySpan" ||
                    n == "ReadOnlyStringSpan")
                    return arraySpanStructType();
                // Heap-pointer types — all stdlib collection/interface/task names.
                if (n == "Array"       || n == "OwnedArray"     ||
                    n == "Dictionary"  || n == "OwnedDictionary" ||
                    n == "Stack"       || n == "OwnedStack"      ||
                    n == "Queue"       || n == "OwnedQueue"      ||
                    n == "Set"         || n == "OwnedSet"        ||
                    n == "LinkedList"  || n == "OwnedLinkedList" ||
                    n == "LinkedListNode"                        ||
                    n == "StringBuilder" || n == "Random"       ||
                    n == "IEnumerable"   || n == "IEnumerator"  ||
                    n == "OwnedEnumerable" || n == "OwnedEnumerator" ||
                    n == "IAsyncEnumerable" || n == "IAsyncEnumerator" ||
                    n == "OwnedAsyncEnumerable" || n == "OwnedAsyncEnumerator" ||
                    n == "IReadOnlyCollection" || n == "IReadOnlyList" ||
                    n == "ICollection" || n == "IList"           ||
                    n == "IOwnedCollection" || n == "IOwnedList" ||
                    n == "Task"        || n == "OwnedTask"       ||
                    n == "Subscription"|| n == "IntPtr"          ||
                    n == "UIntPtr"     || n == "CString"         ||
                    n == "WString"     || n == "RawBytes"        ||
                    n == "CancellationToken" ||
                    n == "CancellationTokenSource" ||
                    // §28.13 / stdlib §2.16 (#620): imported Shared<T>/Weak<T>
                    // (type name carried in importedName — same fix as the
                    // astNode path above).
                    n == "Shared" || n == "Weak" ||
                    // §30.14/§30.15 (#600 phase c): imported Channel<T>/ChannelSender<T>.
                    n == "Channel" || n == "ChannelSender" ||
                    // stdlib §5 (#651): imported IGrouping<TKey, T>.
                    n == "IGrouping" ||
                    // §24.3 (#747): an IEvent<T>/IAsyncEvent<T> value is the
                    // bare bz_event_source_t* (a borrow of the declaring
                    // instance's backing source).
                    n == "IEvent" || n == "IAsyncEvent" ||
                    // §6.2 / #647: imported Mutex (bz_mutex_t*) and Lock (bz_lock_t*).
                    n == "Mutex" || n == "Lock" ||
                    // §6.3 / #647: imported Semaphore / SemaphoreToken.
                    n == "Semaphore" || n == "SemaphoreToken" ||
                    // §6.4 / #647: imported Atomic<T> and AtomicInteger<T>.
                    n == "Atomic" || n == "AtomicInteger" ||
                    // issue #545: imported Buffer<T> (bz_buffer_hdr_t*).
                    n == "Buffer") {
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
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty() &&
                builtinVtable_.find(
                    std::string(rt->astNode->name->parts.back())) !=
                    builtinVtable_.end())
                return ptrTy();
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
                auto* rtd = dynamic_cast<const ast::TypeDecl*>(
                    rt->declaration->decl);
                if (!rt->genericArgs.empty() && rtd &&
                    rtd->genericParams.size() == rt->genericArgs.size()) {
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
                            genericSubst_.count(arg->astNode->name->parts[0])) {
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
                    for (std::size_t gi = 0;
                         gi < rtd->genericParams.size(); ++gi) {
                        newSubst[rtd->genericParams[gi].name] =
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
                        newSema[rtd->genericParams[gi].name] =
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
            // stdlib §5 (#758): a lazy-LINQ enumerable sentinel TypeRef (Named
            // kind, null declaration/astNode, empty importedName, non-empty
            // genericArgs) is a heap `bz_lazy_t*`, not a value type — it falls
            // through every guard above to this value-kind fallback and would
            // wrongly lower to i32, declaring a SelectMany-style selector lambda
            // `(n) => arr.Where(...)` with an i32 return while its body returns
            // a `ptr` (the IR_FAIL "return type does not match operand type").
            // Lower it to a pointer. // per stdlib §5
            if (isLazyEnumerableType(rt)) return ptrTy();
            return i32();  // other value-kind named — int
        case sema::TypeRef::Kind::InlineArray: {
            // §15.1: [N]T is an LLVM fixed-size array [N x elemType].
            // N is an IntLiteralExpr, or a folded compile-time size (#734:
            // arraySizeValue, set by sema for a member-`const` size); element
            // type recurses.
            if (!rt->arrayElement || !rt->arraySize) return i32();
            llvm::Type* elem = lowerTypeRef(rt->arrayElement);
            if (auto* lit =
                    dynamic_cast<const ast::IntLiteralExpr*>(rt->arraySize))
                return llvm::ArrayType::get(
                    elem, std::stoull(std::string(lit->text)));
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
    // §9.1: a record's positional field carries its type on the
    // originating Param, not a FieldDecl (its `decl` is the TypeDecl).
    if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m))
        if (fs->posParam)
            return lowerType(fs->posParam->type.get());
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
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
        return lowerType(fd->type.get());
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
        return lowerType(pd->type.get());
    // per §24.1: an event's backing EventSource<T> field is a runtime object
    // referenced by an opaque pointer.
    if (dynamic_cast<const sema::EventSymbol*>(m))
        return ptrTy();
    return i32();
}


llvm::StructType* Emitter::classStruct(const sema::TypeSymbol* t) {
    // CG29: a record has no vtable header; route to recordStruct so every
    // classStruct caller (field access, etc.) gets the right layout.
    if (t->kind == ast::TypeDeclKind::Record) return recordStruct(t);

    // CG-generic: if we have an active substitution, compute the concrete
    // field types first and look them up in the specialization cache.
    if (!genericSubst_.empty()) {
        // Peek at what field types we'd get with the substitution.
        std::vector<const sema::Symbol*> storage;
        collectInstanceStorage(t, storage);
        std::vector<llvm::Type*> specFieldTys;
        specFieldTys.push_back(ptrTy());            // [0] vtable pointer
        // #595: a class that directly inherits a built-in exception inserts the
        // inherited message-ptr slot at [1] so byte offset 8 matches the built-in
        // exception layout (vtable + message_ptr at offset 0/8).  The disposed
        // flag moves to [2] in that case.
        if (hasBuiltinExceptionBase(t))
            specFieldTys.push_back(ptrTy());        // [1] inherited message ptr
        specFieldTys.push_back(disposedFlagTy());   // [1 or 2] disposed run-once flag
        for (const sema::Symbol* m : storage)
            specFieldTys.push_back(storageType(m));
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
                auto* btd = dynamic_cast<const ast::TypeDecl*>(base->decl);
                if (!baseRef || !btd ||
                    btd->genericParams.size() != baseRef->genericArgs.size())
                    break;  // unresolvable — fall through to unspecialized layout
                for (std::size_t gi = 0; gi < btd->genericParams.size(); ++gi) {
                    const sema::TypeRef* arg = baseRef->genericArgs[gi];
                    // Resolve a bare generic-param ref (`T`, no declaration)
                    // through the lower level's bindings; concrete args pass
                    // through unchanged.
                    if (arg && arg->kind == sema::TypeRef::Kind::Named &&
                        !arg->declaration && arg->astNode && arg->astNode->name &&
                        arg->astNode->name->parts.size() == 1) {
                        auto it = curArgs.find(arg->astNode->name->parts[0]);
                        if (it != curArgs.end()) arg = it->second;
                    }
                    std::string_view pname = btd->genericParams[gi].name;
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
    // #595 / §12 / §31: a user class that directly inherits a built-in exception
    // (e.g. `class ErrA: Exception`) needs to share the Exception object layout
    // so that `e.Message` reads the same slot regardless of whether `e` is typed
    // as the user class or as the built-in base.  Built-in exceptions are laid out
    // as [vtable_ptr, message_ptr] (16 raw bytes, no TypeSymbol, no disposed flag).
    // We honour that layout by inserting the message-ptr at slot 1 BEFORE the
    // disposed flag, giving: [vtable(0), message_ptr(1), disposed(2), own_fields…].
    // disposedFlagIndex() returns 2 for such classes so the disposer and disposal
    // checks GEP to the right slot automatically.
    if (hasBuiltinExceptionBase(t))
        fieldTys.push_back(ptrTy());          // [1] inherited message ptr (ptr)
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
// For user classes that directly inherit a built-in exception type (§12 / §31):
// slot 2 — because slot 1 is occupied by the inherited message-ptr that mirrors
// the built-in exception's [vtable, message_ptr] layout at byte offsets 0 and 8.
// Moving the disposed flag to slot 2 ensures byte offset 8 always carries the
// message pointer, so `e.Message` reads the same slot whether `e` is typed as
// the user class or as the built-in Exception base type.
unsigned Emitter::disposedFlagIndex(const sema::TypeSymbol* t) {
    if (t && hasBuiltinExceptionBase(t)) return 2u;
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
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration ||
        rt->declaration->kind != ast::TypeDeclKind::Record ||
        rt->genericArgs.empty())
        return;
    auto* rtd = dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl);
    if (rtd && rtd->genericParams.size() == rt->genericArgs.size()) {
        // Local generic record: generic param names come from the ast::TypeDecl.
        savedSubst = e->genericSubst_;
        savedSemaTy = e->genericSubstSemaTy_;
        std::unordered_map<std::string_view, llvm::Type*> newSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
        for (std::size_t gi = 0; gi < rtd->genericParams.size(); ++gi) {
            newSubst[rtd->genericParams[gi].name] =
                e->lowerTypeRef(rt->genericArgs[gi]);
            // §14.1 (#766): resolve a bare-param arg through the outer subst.
            newSema[rtd->genericParams[gi].name] = e->resolveTypeParamRef(rt->genericArgs[gi]);
        }
        e->genericSubst_ = std::move(newSubst);
        e->genericSubstSemaTy_ = std::move(newSema);
        active = true;
    } else if (!rtd &&
               rt->declaration->genericParams.size() == rt->genericArgs.size()) {
        // #428 / §14.1 / §2.5: IMPORTED generic record — no ast::TypeDecl, but
        // generic param names are in TypeSymbol::genericParams (populated by
        // ModuleLoader::convertGenericParams). Build the substitution the same
        // way so field accesses on an imported Box<string> get the ptr layout.
        savedSubst = e->genericSubst_;
        savedSemaTy = e->genericSubstSemaTy_;
        std::unordered_map<std::string_view, llvm::Type*> newSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
        for (std::size_t gi = 0; gi < rt->declaration->genericParams.size(); ++gi) {
            std::string_view pname = rt->declaration->genericParams[gi]->name;
            newSubst[pname] = e->lowerTypeRef(rt->genericArgs[gi]);
            // §14.1 (#766): resolve a bare-param arg through the outer subst.
            newSema[pname] = e->resolveTypeParamRef(rt->genericArgs[gi]);
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
        auto it = genericSubstSemaTy_.find(declared->astNode->name->parts[0]);
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
    return nullablePrimStructTy(rt->primitive);
}


// fix #568 / §14.1 / §7.1: see the declaration comment in LlvmBackend_impl.hpp.
llvm::Value* Emitter::coerceReturnToNullableValue(llvm::Value* v,
                                                  const sema::TypeRef* declRet,
                                                  bool srcUnsigned) {
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

    llvm::Type* slotTy = nst->getElementType(1);
    // #843 / §5.1 / §7: widen/convert a NARROWER numeric return value to the
    // nullable's inner type before wrapping — `return 42` from a `long?` method
    // needs the i32 literal i32→i64, `return someInt` from a `double?` method
    // needs SIToFP. Without this the bare narrower value fell through unwrapped
    // and emitReturn then `ret`'d it against the `{i8, T}` struct return type
    // (IR-verify failure). Mirrors the local-decl fix (#842).
    if ((v->getType()->isIntegerTy() || v->getType()->isFloatingPointTy()) &&
        v->getType() != slotTy) {
        llvm::Value* inner = coerceNumericToType(v, slotTy, srcUnsigned);
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
    llvm::Value* obj = lowerExpr(*te.operand);
    if (!obj) return nullptr;
    sema::TypeRef* rt = ctx_.resolvedType(*te.testType);
    if (!rt) return nullptr;
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
        if (!obj->getType()->isPointerTy()) return nullptr;
        auto* isStrFn = declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
        llvm::Value* isStr = builder_.CreateCall(
            isStrFn, {obj, vtableTablePtr()}, "is.string");
        llvm::Value* isStrBit =
            builder_.CreateICmpNE(isStr, builder_.getInt32(0));
        if (te.op == "is") return isStrBit;
        if (te.op == "is not")
            return builder_.CreateXor(isStrBit, builder_.getInt1(true));
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
        if (!obj->getType()->isPointerTy()) return nullptr;
        llvm::Value* nonNull = builder_.CreateICmpNE(
            obj, llvm::ConstantPointerNull::get(ptrTy()), "is.object");
        if (te.op == "is") return nonNull;
        if (te.op == "is not")
            return builder_.CreateXor(nonNull, builder_.getInt1(true));
        if (te.op == "as")
            return builder_.CreateSelect(
                nonNull, obj, llvm::ConstantPointerNull::get(ptrTy()));
        return nullptr;
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
    if (!targetDesc) return nullptr;
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
        const sema::TypeRef* operandTy = ctx_.exprType(*te.operand);
        if (operandTy && isNullableValueTy(operandTy) &&
            obj->getType()->isStructTy()) {
            llvm::Value* hasVal = nullablePrimHasValue(obj);  // i1
            if (te.op == "is") return hasVal;
            if (te.op == "is not") return builder_.CreateNot(hasVal);
        }
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
    if (const sema::TypeRef* opTy = ctx_.exprType(*te.operand)) {
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
    if (te.op == "is") return isA;
    if (te.op == "is not")
        return builder_.CreateXor(isA, builder_.getInt1(true));
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
        if (isBoxableValueType(rt)) {
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
            if (rt->kind == sema::TypeRef::Kind::Primitive ||
                isNullableRecordTy(rt) ||
                (rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
                 (rt->declaration->kind == ast::TypeDeclKind::Record ||
                  rt->declaration->kind == ast::TypeDeclKind::Enum))) {
                // §8.6 / §17.4 (issue #416, enum #1072): a value primitive,
                // record, or ENUM unboxes to a non-null T? on a compatible cast,
                // a null T? otherwise. The result is the tagged {hasValue,
                // payload} struct: hasValue=1 + the loaded payload on success, the
                // zero struct on failure. For a record the payload is the bare
                // record struct; for a primitive/enum it is the scalar (the enum's
                // underlying integer, §10.3). `rt` is the (non-nullable) target
                // type, so build the nullable form's tagged struct — without this
                // an `enum?` from `as` was a bare value, and the downstream
                // `int(enum?)` / `<> null` / `!` GEP'd its tag on a non-struct
                // (#1072 LLVM checkGEPType assertion).
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

    if (!nullableValue) return emitBox();

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
    return phi;
}


// §8.6: box `v` (the lowered value of `init`) when the declared location
// `declTy` is Object/interface and the source expression is a value type.
// A no-op for reference sources and non-Object/interface targets — the #399
// reference-type Object path (a class pointer coerced to Object stays a bare
// pointer) is preserved by only boxing boxable VALUE sources.
llvm::Value* Emitter::boxForTarget(llvm::Value* v, const sema::TypeRef* declTy,
                                   const ast::Expr* init) {
    if (!v || !declTy || !init) return v;
    if (!isObjectOrInterfaceTarget(declTy)) return v;
    const sema::TypeRef* srcTy = ctx_.exprType(*init);
    if (!srcTy) return v;
    // Only box a value-typed source. A reference source (class/interface/
    // string/Object) already lowers to a pointer and reaches Object directly.
    if (!isBoxableValueType(srcTy) && !isNullableValueTy(srcTy)) return v;
    // The lowered value must be a scalar/struct (a value), not already a ptr
    // (which would mean it was lowered as a reference — leave it alone).
    if (v->getType()->isPointerTy() && !isNullableValueTy(srcTy)) return v;
    return boxValue(v, srcTy);
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
llvm::Value* Emitter::boxedStorage(llvm::AllocaInst* slot, llvm::Type*& elemTy) {
    auto it = cellVars_.find(slot);
    if (it != cellVars_.end()) {
        elemTy = it->second;
        // The alloca holds a pointer to the heap cell; the value lives there.
        return builder_.CreateLoad(ptrTy(), slot, "cell");
    }
    elemTy = slot->getAllocatedType();
    return slot;
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
        newSlot = slot;
        if (wasGuardedBoxing) *wasGuardedBoxing = false; // treat as non-guarded (already boxed)
        return builder_.CreateLoad(ptrTy(), slot, "cell");
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
                             llvm::Value* receiverVal) {
    llvm::Function* origFn = nullptr;
    auto it = methodFns_.find(ms);
    if (it != methodFns_.end()) origFn = it->second;
    // §27.6 (#738): a method reference to a VIRTUAL or INTERFACE method dispatches
    // DYNAMICALLY through the receiver's vtable/itable when invoked (not the
    // statically-resolved origFn). An INTERFACE method has no concrete body /
    // methodFns_ entry, so derive its LLVM signature from methodFnType.
    bool instMethod = !isStaticMethod(ms);
    auto ifaceIt = ifaceMethodIndex_.find(ms);
    auto vtSlotIt = vtableSlot_.find(ms);
    bool dynamicDispatch = instMethod &&
        (ifaceIt != ifaceMethodIndex_.end() || vtSlotIt != vtableSlot_.end());
    llvm::FunctionType* methTy =
        origFn ? origFn->getFunctionType() : methodFnType(ms);
    if (!methTy) return nullptr;
    // A non-virtual, non-interface method ref must have a concrete origFn.
    if (!origFn && !dynamicDispatch) return nullptr;

    // Build the thunk's parameter types: (ptr env, user_params...).
    std::vector<llvm::Type*> thunkParamTys;
    thunkParamTys.push_back(ptrTy());  // env_ptr
    // Skip the leading `this` arg for instance methods
    // (it comes from env instead).
    for (std::size_t i = (instMethod ? 1 : 0);
         i < methTy->getNumParams(); ++i)
        thunkParamTys.push_back(methTy->getParamType(i));
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
    builder_.SetInsertPoint(
        llvm::BasicBlock::Create(llvm_, "entry", thunk));

    // Build call args to the original function.
    std::vector<llvm::Value*> callArgs;
    auto argIt = thunk->args().begin();
    llvm::Value* envPtrArg = &*argIt++;  // env_ptr (arg 0)
    if (instMethod) {
        // §26.1 / #457: Load receiver from env with the CORRECT `this` type.
        // For a class/interface/record extension method `this` is a ptr (the
        // env block holds a ptr and we load ptr). For a PRIMITIVE or ENUM
        // extension method `this` is passed BY VALUE (e.g. i32 for `int`);
        // the env block holds the raw value bytes and we must load that type.
        // Always loading ptr (the old code) produces a type mismatch that the
        // IR verifier catches and that SIGSEGVs at call time.
        llvm::Type* thisLoadTy = ptrTy();  // default: pointer (class/record)
        if (isExtensionMethod(ms)) {
            // The actual `this` type is origFn->getArg(0)->getType().
            llvm::Type* extThisTy = origFn->getArg(0)->getType();
            if (!extThisTy->isPointerTy()) thisLoadTy = extThisTy;
        }
        callArgs.push_back(builder_.CreateLoad(thisLoadTy, envPtrArg));
    }
    for (; argIt != thunk->args().end(); ++argIt)
        callArgs.push_back(&*argIt);

    // §27.6 (#738): dispatch through the receiver's vtable/itable for a virtual
    // or interface method (mirrors emitMethodCall's dynamic path); otherwise call
    // the concrete origFn directly. callArgs[0] is the receiver.
    llvm::Value* dynFnPtr = nullptr;
    if (dynamicDispatch && !callArgs.empty() &&
        callArgs[0]->getType()->isPointerTy()) {
        llvm::Value* recv = callArgs[0];
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
    if (retTy->isVoidTy()) builder_.CreateRetVoid();
    else builder_.CreateRet(result);

    // Restore emitter state.
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals); paramSlots_ = std::move(savedParams);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    unwindTarget_ = savedUnwind; propagateBB_ = savedProp;
    currentCaughtExc_ = savedCaught; unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    if (savedBB) builder_.SetInsertPoint(savedBB);

    // Build closure cell { thunk_ptr, env_ptr, rc, envdtor }.
    // #422 / #672 / §27.6 / §28.9: a method reference is a runtime-managed
    // function value backed by the same closure cell as a lambda (now 32 bytes:
    // rc at off 16, envdtor at off 24 — #672 foundation). The env is null for a
    // static method, or an 8-byte block holding the receiver (a BORROW of the
    // receiver, §28.3 — the cell does not own it). bz_closure_release frees the
    // cell + env block, never the receiver. envdtor is null (foundation: the env
    // for a method thunk holds a borrow and needs no typed disposal).
    // For a primitive/enum extension receiver the env block stores the raw value
    // bytes (≤ 8 bytes for all supported primitives).
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
    unwindTarget_ = nullptr; propagateBB_ = nullptr;
    currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0; diverged_ = false;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", thunk));

    // Load the captured receiver from env; bind as `this`.
    llvm::Value* thisVal = builder_.CreateLoad(ptrTy(), thunk->getArg(0));
    currentThis_ = thisVal;

    // §14.1: for a generic class receiver, set up the generic substitution so a
    // getter body that reads a `T`-typed field uses the right struct layout.
    // (Mirror the computed-getter read path in LlvmBackend_Exprs.cpp.)
    if (ma && ma->target && ps->parent && ps->parent->decl) {
        if (sema::TypeRef* recvTy = ctx_.exprType(*ma->target)) {
            if (recvTy->kind == sema::TypeRef::Kind::Named &&
                recvTy->declaration == ps->parent && !recvTy->genericArgs.empty()) {
                auto* td = dynamic_cast<const ast::TypeDecl*>(ps->parent->decl);
                if (td && td->genericParams.size() == recvTy->genericArgs.size()) {
                    std::unordered_map<std::string_view, llvm::Type*> newSubst;
                    std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
                    for (std::size_t gi = 0; gi < td->genericParams.size(); ++gi) {
                        newSubst[td->genericParams[gi].name] =
                            lowerTypeRef(recvTy->genericArgs[gi]);
                        newSema[td->genericParams[gi].name] =
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
            result = lowerExpr(*getter);
        } else if (const ast::PropertyAccessor* acc = blockGetterOf(ps)) {
            if (llvm::Function* fn = ensurePropertyBlockGetter(pd, acc)) {
                result = builder_.CreateCall(fn->getFunctionType(), fn, {thisVal});
                emitPendingCheck();
            }
        } else if (isAutoProperty(ps)) {
            auto idxIt = fieldIndex_.find(ps);
            if (idxIt != fieldIndex_.end()) {
                llvm::StructType* st = classStruct(ps->parent);
                llvm::Type* fty = st->getElementType(idxIt->second);
                result = builder_.CreateLoad(
                    fty, builder_.CreateStructGEP(st, thisVal, idxIt->second));
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
    unwindTarget_ = savedUnwind; propagateBB_ = savedProp;
    currentCaughtExc_ = savedCaught; unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (savedBB) builder_.SetInsertPoint(savedBB);

    // Build the closure cell { thunk_ptr, env_ptr, rc, envdtor } at the call
    // site. The env holds the receiver as a BORROW (§28.3); bz_closure_release
    // frees the cell + env, never the receiver.
    uint64_t cellSz =
        module_->getDataLayout().getTypeAllocSize(closureCellType());
    llvm::Value* closureObj = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), builder_.getInt64((int64_t)cellSz)});
    builder_.CreateStore(thunk, closureObj);
    llvm::Value* envObj = llvm::ConstantPointerNull::get(ptrTy());
    if (receiverVal && receiverVal->getType()->isPointerTy()) {
        envObj = builder_.CreateCall(
            callocFn(), {builder_.getInt64(1), builder_.getInt64(8)});
        builder_.CreateStore(receiverVal, envObj);
    }
    builder_.CreateStore(
        envObj,
        builder_.CreateGEP(ptrTy(), closureObj, {builder_.getInt32(1)}));
    initClosureRc(closureObj);
    initClosureEnvdtor(closureObj);
    return closureObj;
}


sema::MemberSymbol* Emitter::storageMemberOf(sema::Symbol* sym) {
    if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym)) return fs;
    if (isAutoProperty(sym))
        return dynamic_cast<sema::PropertySymbol*>(sym);
    return nullptr;
}


llvm::Value* Emitter::implicitFieldAddr(const ast::Expr& e, llvm::Type*& elemTy) {
    if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
    auto* ms = storageMemberOf(ctx_.exprSymbol(e));
    if (!ms || !ms->parent || !currentThis_) return nullptr;
    auto idxIt = fieldIndex_.find(ms);
    if (idxIt == fieldIndex_.end()) return nullptr;  // static field
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
        // Recover the enclosing class: reverse-look up currentFn_ in methodFns_,
        // then try currentSpecMethodSymbol_ (for specialised bodies).
        const sema::TypeSymbol* enclosingCls = nullptr;
        if (currentFn_) {
            for (const auto& [sym, fn] : methodFns_)
                if (fn == currentFn_ && sym->parent) {
                    enclosingCls = sym->parent;
                    break;
                }
        }
        if (!enclosingCls && currentSpecMethodSymbol_ &&
            currentSpecMethodSymbol_->parent)
            enclosingCls = currentSpecMethodSymbol_->parent;
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
    if (fieldTy && fieldTy->kind == K::Named && fieldTy->declaration &&
        fieldTy->declaration->kind == ast::TypeDeclKind::Record &&
        l->getType()->isStructTy()) {
        if (auto* nested = llvm::dyn_cast<llvm::StructType>(l->getType()))
            return emitStructEquality(l, r, nested, fieldTy->declaration);
    }
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
                auto cit = genericSubstSemaTy_.find(tt->astNode->name->parts[0]);
                if (cit != genericSubstSemaTy_.end() && cit->second &&
                    cit->second->kind == sema::TypeRef::Kind::Named &&
                    cit->second->declaration) {
                    for (sema::Symbol* m : cit->second->declaration->members) {
                        if (!m || m->name != ma.member) continue;
                        ms = storageMemberOf(m);
                        if (ms) break;
                    }
                }
            }
        }
    }
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
            auto* td = dynamic_cast<const ast::TypeDecl*>(
                ms->parent->decl);
            if (td && td->genericParams.size() ==
                      targetTy->genericArgs.size()) {
                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*>
                    newSema;
                for (std::size_t gi = 0;
                     gi < td->genericParams.size(); ++gi) {
                    newSubst[td->genericParams[gi].name] =
                        lowerTypeRef(targetTy->genericArgs[gi]);
                    // §14.1 (#786): resolve a bare-param arg through the outer
                    // subst (built BEFORE genericSubstSemaTy_ is replaced).
                    newSema[td->genericParams[gi].name] =
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
                for (std::size_t gi = 0;
                     gi < td->genericParams.size(); ++gi) {
                    const sema::TypeRef* arg = targetTy->genericArgs[gi];
                    if (arg && arg->kind == sema::TypeRef::Kind::Named &&
                        !arg->declaration && arg->astNode &&
                        arg->astNode->name &&
                        arg->astNode->name->parts.size() == 1) {
                        hasUnresolved = true;
                        break;
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
                genericSubst_ = std::move(newSubst);
                genericSubstSemaTy_ = std::move(newSema);
            } else if (!td &&
                       ms->parent->genericParams.size() ==
                       targetTy->genericArgs.size()) {
                // #428 / §14.1 / §2.5: IMPORTED generic type — no ast::TypeDecl,
                // but TypeSymbol::genericParams has the param names.  Build the
                // same layout substitution so recordStruct (and classStruct)
                // produce the correct specialized struct for field-index mapping.
                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*>
                    newSema;
                for (std::size_t gi = 0;
                     gi < ms->parent->genericParams.size(); ++gi) {
                    newSubst[ms->parent->genericParams[gi]->name] =
                        lowerTypeRef(targetTy->genericArgs[gi]);
                    // §14.1 (#786): build the parallel sema-type map (#766).
                    newSema[ms->parent->genericParams[gi]->name] =
                        resolveTypeParamRef(targetTy->genericArgs[gi]);
                }
                genericSubst_ = std::move(newSubst);
                genericSubstSemaTy_ = std::move(newSema);
            }
        } else if (targetTy->kind == sema::TypeRef::Kind::Named &&
                   targetTy->declaration &&
                   targetTy->declaration != ms->parent &&
                   !targetTy->genericArgs.empty() &&
                   !ms->parent->genericParams.empty()) {
            // §12 / §14 (issue #585): direct access of an INHERITED field
            // declared on a generic BASE (`ms->parent`) through a derived
            // instance (`targetTy` == Wrapper<Pt>). The branch above requires
            // targetTy->declaration == ms->parent, so it does not fire — without
            // a substitution classStruct(ms->parent) uses the T→i32 template
            // layout and the GEP/field type are wrong for a non-int base field
            // (read 0 / garbage). Bind the base's generic params to the concrete
            // arguments the derived class supplied (walking the base chain).
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
        if (llvm::AllocaInst* slot = localSlot(*ma.target)) {
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
