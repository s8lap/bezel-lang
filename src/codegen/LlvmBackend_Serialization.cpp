// LlvmBackend_Serialization.cpp — §9.8 / §2.14–2.15 synthesis codegen.
//
// Synthesised Serialize / Deserialize for records and enums (tier 1: tree
// shape, Binary format only).  BinarySerializer / BinaryDeserializer
// construction, ToBytes, AsReadOnlySpan, and Dispose forwarding.
//
// Spec: lang §9.8; stdlib §2.14 (ISerializer/IDeserializer), §2.15
// (BinarySerializer/BinaryDeserializer), §4.14 (OwnedBuffer<T>/Span<T>).
// Issue #550, tier 1.

#include "codegen/LlvmBackend_impl.hpp"

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bezel::codegen {

using namespace bezel::sema;
using namespace bezel::ast;

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

// Test whether a TypeSymbol's base-type list mentions the given name.
static bool declaresConformance(const TypeSymbol* ts, std::string_view name) {
    if (!ts || !ts->decl) return false;
    const auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    if (!td) return false;
    for (const auto& bt : td->baseTypes) {
        if (!bt) continue;
        if (bt->name && !bt->name->parts.empty() &&
            bt->name->parts[0] == name)
            return true;
    }
    return false;
}

// Test whether a TypeSymbol has a hand-written method of the given name.
static bool hasHandWrittenMethod(const TypeSymbol* ts, std::string_view name) {
    if (!ts) return false;
    for (const Symbol* m : ts->members) {
        if (m->name == name)
            if (dynamic_cast<const MethodSymbol*>(m))
                return true;
    }
    return false;
}

// Return the wire name for a positional field (either the `serialized "x"` name
// or the declared identifier).
static std::string_view positionalFieldWireName(const ast::Param& pf) {
    if (pf.serialized && !pf.serialized->name.empty())
        return pf.serialized->name;
    return pf.name;
}

// Returns true iff the positional field is marked `serialized skip`.
static bool positionalFieldIsSkip(const ast::Param& pf) {
    return pf.serialized && pf.serialized->isSkip;
}

// Return the wire name for an enum constant.  May be overridden via
// `serialized "..."` on the EnumConstant node.
static std::string_view enumConstWireName(const ast::EnumConstant& ec) {
    if (ec.serialized && !ec.serialized->name.empty())
        return ec.serialized->name;
    return ec.name;
}

// ---------------------------------------------------------------------------
// Emitter::emitRecordSerialize
//
// Emits an inline synthesized `Serialize(s: ISerializer)` body for a
// positional record value `recvVal` (an LLVM struct value) and a serializer
// handle `serPtr` (an opaque ptr — a bz_binser_t*).  Returns void.
//
// Per §9.8 synthesized Serialize:
//   1. s.BeginObject(n)                         — write count
//   2. for each non-skip field:
//        s.WriteFieldName("<name>")
//        s.<Write*>(field_value)
//   3. s.EndObject()
// ---------------------------------------------------------------------------
void Emitter::emitRecordSerialize(const TypeSymbol* ts,
                                  llvm::Value* recvVal,
                                  llvm::Value* serPtr) {
    if (!ts || !ts->decl) return;
    const auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    if (!td) return;

    // Collect serialized components (positional fields, skip excluded).
    std::vector<const ast::Param*> components;
    if (td->hasPositionalFields) {
        for (const auto& pf : td->positionalFields)
            if (!positionalFieldIsSkip(pf))
                components.push_back(&pf);
    } else {
        // Body record: auto-properties in declaration order.
        // (Handled via collectRecordStorage below — use the same index.)
        // Nothing to add to `components`; we use the storage vector below.
    }

    // Collect storage symbols for index mapping.
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(ts, storage);

    llvm::Type* voidTy  = llvm::Type::getVoidTy(llvm_);

    // BeginObject(field_count)
    int32_t nonSkipCount = (int32_t)components.size();
    if (!td->hasPositionalFields) nonSkipCount = (int32_t)storage.size();  // body
    auto* beginFn = declareC("bz_binser_begin_object", voidTy, {ptrTy(), i32()});
    builder_.CreateCall(beginFn, {serPtr, builder_.getInt32(nonSkipCount)});

    // Per-field: WriteFieldName + Write
    unsigned storageIdx = 0;
    for (unsigned mi = 0; mi < storage.size(); ++mi) {
        const sema::Symbol* sym = storage[mi];
        std::string wireName;

        // Determine wire name and skip flag.
        bool skip = false;
        if (td->hasPositionalFields) {
            // Positional fields: find corresponding entry in td->positionalFields.
            // The storage vector order matches td->positionalFields order.
            if (storageIdx < td->positionalFields.size()) {
                const ast::Param& pf = td->positionalFields[storageIdx];
                if (positionalFieldIsSkip(pf)) { ++storageIdx; continue; }
                wireName = std::string(positionalFieldWireName(pf));
                ++storageIdx;
            } else {
                ++storageIdx;
                continue;
            }
        } else {
            // Body record: auto-property; check its PropertyDecl for serialized clause.
            if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(sym->decl)) {
                if (pd->serialized && pd->serialized->isSkip) continue;
                if (pd->serialized && !pd->serialized->name.empty())
                    wireName = std::string(pd->serialized->name);
                else
                    wireName = std::string(sym->name);
            } else {
                wireName = std::string(sym->name);
            }
        }
        if (skip) continue;

        llvm::Value* fieldVal = builder_.CreateExtractValue(recvVal, {mi});
        const sema::TypeRef* fieldTy = recordMemberTypeResolved(sym);

        // WriteFieldName (bz_string* for the wire name)
        llvm::Value* nameStr = lowerStringLiteral(
            std::string("\"") + wireName + "\"", false);
        auto* writeNameFn = declareC("bz_binser_write_field_name", voidTy,
                                     {ptrTy(), ptrTy()});
        builder_.CreateCall(writeNameFn, {serPtr, nameStr});

        // Write value
        emitSerializeField(fieldVal, fieldTy, serPtr);
    }

    // EndObject
    auto* endFn = declareC("bz_binser_end_object", voidTy, {ptrTy()});
    builder_.CreateCall(endFn, {serPtr});
}

// ---------------------------------------------------------------------------
// Emitter::emitSerializeField
//
// Emit the write of a single field value of Bezel type `fieldTy` into
// serializer `serPtr`.  Dispatches on primitive kind, string, enum, nested
// record.  Called by emitRecordSerialize.
// ---------------------------------------------------------------------------
void Emitter::emitSerializeField(llvm::Value* val,
                                 const sema::TypeRef* fieldTy,
                                 llvm::Value* serPtr) {
    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);
    using K = sema::TypeRef::Kind;

    if (!fieldTy) {
        // Unknown type: skip (no-op write — partial fault, should not reach).
        return;
    }

    // §9.8 / #638 / #637: nullable check MUST come before Primitive/Named dispatch.
    // TypeRef represents T? with the same kind as T but nullable=true.  If we
    // checked K::Primitive first, an int?/{i8,i32} struct value would reach the
    // integer SExt path → LLVM verifier failure.  A string? nullptr would be
    // passed to bz_binser_write_string which writes a zero-length string (not a
    // null marker) → Deserialize reads "" instead of null (#637).
    if (fieldTy->nullable) {
        if (val->getType()->isPointerTy()) {
            // Nullable reference type (string?, class?): ptr is null → write null.
            llvm::Value* isNull = builder_.CreateICmpEQ(
                val, llvm::ConstantPointerNull::get(ptrTy()), "ser.isnull");
            auto* nullBB = llvm::BasicBlock::Create(llvm_, "ser.null", currentFn_);
            auto* liveBB = llvm::BasicBlock::Create(llvm_, "ser.live", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "ser.nend", currentFn_);
            builder_.CreateCondBr(isNull, nullBB, liveBB);
            builder_.SetInsertPoint(nullBB);
            {
                auto* fn = declareC("bz_binser_write_null", voidTy, {ptrTy()});
                builder_.CreateCall(fn, {serPtr});
            }
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(liveBB);
            {
                // Write the non-null value: strip nullable flag and recurse.
                sema::TypeRef nonNullTy = *fieldTy;
                nonNullTy.nullable = false;
                emitSerializeField(val, &nonNullTy, serPtr);
            }
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            return;
        }
        // Nullable value type: {i8 hasValue; T value} struct layout.
        if (val->getType()->isStructTy()) {
            llvm::Value* hasVal = builder_.CreateExtractValue(val, {0u}, "ser.hasv");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                hasVal, builder_.getInt8(0), "ser.isnull");
            auto* nullBB = llvm::BasicBlock::Create(llvm_, "ser.null", currentFn_);
            auto* liveBB = llvm::BasicBlock::Create(llvm_, "ser.live", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "ser.nend", currentFn_);
            builder_.CreateCondBr(isNull, nullBB, liveBB);
            builder_.SetInsertPoint(nullBB);
            {
                auto* fn = declareC("bz_binser_write_null", voidTy, {ptrTy()});
                builder_.CreateCall(fn, {serPtr});
            }
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(liveBB);
            {
                llvm::Value* inner = builder_.CreateExtractValue(val, {1u}, "ser.inner");
                sema::TypeRef nonNullTy = *fieldTy;
                nonNullTy.nullable = false;
                emitSerializeField(inner, &nonNullTy, serPtr);
            }
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            return;
        }
        // Fallback: write null marker.
        auto* fn = declareC("bz_binser_write_null", voidTy, {ptrTy()});
        builder_.CreateCall(fn, {serPtr});
        return;
    }

    if (fieldTy->kind == K::Primitive) {
        switch (fieldTy->primitive) {
        case sema::PrimitiveType::Bool: {
            // bool is i1 in LLVM; widen to i8 for bz_binser_write_bool.
            llvm::Value* b = builder_.CreateZExt(val, i8(), "ser.bool");
            auto* fn = declareC("bz_binser_write_bool", voidTy, {ptrTy(), i8()});
            builder_.CreateCall(fn, {serPtr, b});
            break;
        }
        case sema::PrimitiveType::Char: {
            // char is i32 (Unicode scalar).
            llvm::Value* c = val;
            if (!c->getType()->isIntegerTy(32))
                c = builder_.CreateSExtOrTrunc(c, i32(), "ser.char");
            auto* fn = declareC("bz_binser_write_char", voidTy, {ptrTy(), i32()});
            builder_.CreateCall(fn, {serPtr, c});
            break;
        }
        case sema::PrimitiveType::Byte:
        case sema::PrimitiveType::SByte:
        case sema::PrimitiveType::Short:
        case sema::PrimitiveType::UShort:
        case sema::PrimitiveType::Int:
        case sema::PrimitiveType::Long:
        case sema::PrimitiveType::UInt:
        case sema::PrimitiveType::ULong: {
            // Write as i64 (§2.15: integers use 8-byte LE encoding).
            bool isUnsigned = (fieldTy->primitive == sema::PrimitiveType::Byte ||
                               fieldTy->primitive == sema::PrimitiveType::UShort ||
                               fieldTy->primitive == sema::PrimitiveType::UInt ||
                               fieldTy->primitive == sema::PrimitiveType::ULong);
            llvm::Value* v64 = isUnsigned
                ? builder_.CreateZExt(val, i64(), "ser.uint")
                : builder_.CreateSExt(val, i64(), "ser.int");
            if (isUnsigned) {
                auto* fn = declareC("bz_binser_write_uint", voidTy, {ptrTy(), i64()});
                builder_.CreateCall(fn, {serPtr, v64});
            } else {
                auto* fn = declareC("bz_binser_write_int", voidTy, {ptrTy(), i64()});
                builder_.CreateCall(fn, {serPtr, v64});
            }
            break;
        }
        case sema::PrimitiveType::Float:
        case sema::PrimitiveType::Double: {
            // float → fp-extend to double; double already double.
            llvm::Value* d = val;
            if (fieldTy->primitive == sema::PrimitiveType::Float)
                d = builder_.CreateFPExt(val, llvm::Type::getDoubleTy(llvm_), "ser.dbl");
            auto* fn = declareC("bz_binser_write_float", voidTy,
                                {ptrTy(), llvm::Type::getDoubleTy(llvm_)});
            builder_.CreateCall(fn, {serPtr, d});
            break;
        }
        case sema::PrimitiveType::Decimal: {
            // Decimal: stub — write as double.
            llvm::Value* d = val;
            if (!d->getType()->isDoubleTy())
                d = builder_.CreateBitOrPointerCast(
                    d, llvm::Type::getDoubleTy(llvm_));
            auto* fn = declareC("bz_binser_write_float", voidTy,
                                {ptrTy(), llvm::Type::getDoubleTy(llvm_)});
            builder_.CreateCall(fn, {serPtr, d});
            break;
        }
        case sema::PrimitiveType::String: {
            auto* fn = declareC("bz_binser_write_string", voidTy, {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {serPtr, val});
            break;
        }
        default:
            break;
        }
        return;
    }

    if (fieldTy->kind == K::Named && fieldTy->declaration) {
        const TypeSymbol* fieldTs = fieldTy->declaration;

        if (fieldTs->kind == ast::TypeDeclKind::Enum) {
            // Enum: write member name (§9.8 "writes the member name as a string").
            emitEnumSerialize(fieldTs, val, serPtr);
            return;
        }

        if (fieldTs->kind == ast::TypeDeclKind::Record) {
            // Nested record: recurse.
            // The value should already be a struct (records are value types).
            llvm::StructType* st = recordStruct(fieldTs);
            llvm::Value* recVal = val;
            if (recVal->getType()->isPointerTy())
                recVal = builder_.CreateLoad(st, recVal);
            emitRecordSerialize(fieldTs, recVal, serPtr);
            return;
        }
    }

    // Fallback: unknown / unsupported type — emit null marker (silent skip).
    (void)val;
}

// ---------------------------------------------------------------------------
// Emitter::emitEnumSerialize  §9.8
//
// For enum synthesis: writes the member NAME string (not the integer value).
// If the enum value is not a declared constant, raises SerializationException.
// ---------------------------------------------------------------------------
void Emitter::emitEnumSerialize(const TypeSymbol* ts,
                                llvm::Value* enumVal,
                                llvm::Value* serPtr) {
    if (!ts || !ts->decl) return;
    const auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    if (!td) return;
    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);

    // Extend enum to i32 for comparison.
    llvm::Value* ev = enumVal;
    if (!ev->getType()->isIntegerTy(32))
        ev = builder_.CreateSExtOrTrunc(ev, i32(), "enum.ev");

    // Build a chain of comparisons, one per declared constant.
    // If a constant matches, write its wire name and branch to end.
    // If none match, call bz_ser_throw.
    auto* endBB   = llvm::BasicBlock::Create(llvm_, "enum.ser.end",   currentFn_);
    auto* throwBB = llvm::BasicBlock::Create(llvm_, "enum.ser.throw", currentFn_);

    for (const auto& ec : td->enumConstants) {
        // Determine the constant's integer value using enumValue_ (populated by
        // computeEnumValues(), which handles both local and imported enums).
        int32_t ordinal = -1;
        for (const Symbol* m : ts->members) {
            if (m->name == ec.name) {
                if (dynamic_cast<const EnumConstantSymbol*>(m)) {
                    auto it = enumValue_.find(m);
                    if (it != enumValue_.end()) ordinal = (int32_t)it->second;
                }
                break;
            }
        }
        if (ordinal < 0) continue;  // not found — skip

        std::string wireName = std::string(enumConstWireName(ec));
        llvm::Value* cmpVal  = builder_.getInt32(ordinal);
        llvm::Value* matches = builder_.CreateICmpEQ(ev, cmpVal, "enum.eq");

        auto* writeBB = llvm::BasicBlock::Create(llvm_, "enum.ser.write", currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "enum.ser.next",  currentFn_);
        builder_.CreateCondBr(matches, writeBB, nextBB);

        builder_.SetInsertPoint(writeBB);
        llvm::Value* nameStr = lowerStringLiteral(
            std::string("\"") + wireName + "\"", false);
        auto* writeFn = declareC("bz_binser_write_string", voidTy, {ptrTy(), ptrTy()});
        builder_.CreateCall(writeFn, {serPtr, nameStr});
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(nextBB);
    }

    // No constant matched: throw SerializationException.
    builder_.CreateBr(throwBB);

    builder_.SetInsertPoint(throwBB);
    auto* throwFn = declareC("bz_ser_throw", voidTy, {ptrTy(), ptrTy()});
    llvm::Value* msgStr = builder_.CreateGlobalString(
        "Enum value is not a declared constant - cannot serialize (SS9.8)",
        "ser.exc.msg");
    builder_.CreateCall(throwFn, {msgStr,
        llvm::ConstantPointerNull::get(ptrTy())});
    builder_.CreateBr(endBB);  // unreachable after throw, but needed for IR

    builder_.SetInsertPoint(endBB);
}

// ---------------------------------------------------------------------------
// Emitter::emitRecordDeserialize  §9.8
//
// Emits the synthesized `Deserialize(d: IDeserializer): T` body.
// Returns the constructed record struct value.
//
// Algorithm:
//   1. d.BeginObject()                           — read field count
//   2. alloca locals for each non-skip component
//   3. loop (field_count iterations):
//      a. name := d.ReadFieldName()
//      b. if name matches known component → read into local; release name
//      c. else → d.SkipValue(); release name
//   4. construct the record from the locals
// ---------------------------------------------------------------------------
llvm::Value* Emitter::emitRecordDeserialize(const TypeSymbol* ts,
                                            llvm::Value* derPtr) {
    if (!ts || !ts->decl) return nullptr;
    const auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    if (!td) return nullptr;
    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);

    // Collect serialized components (positional fields, skip excluded).
    struct Component {
        std::string wireName;   // wire name (serialized "x" or identifier)
        std::string declName;   // declared identifier
        const sema::TypeRef* ty;
        bool isSkip = false;
        unsigned storageIdx;    // index into the struct (ExtractValue / InsertValue)
    };
    std::vector<Component> components;
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(ts, storage);

    unsigned si = 0;
    if (td->hasPositionalFields) {
        for (const auto& pf : td->positionalFields) {
            Component comp;
            comp.storageIdx = si;
            comp.declName   = std::string(pf.name);
            comp.isSkip     = positionalFieldIsSkip(pf);
            comp.wireName   = comp.isSkip ? "" : std::string(positionalFieldWireName(pf));
            comp.ty         = si < storage.size()
                ? recordMemberTypeResolved(storage[si]) : nullptr;
            components.push_back(std::move(comp));
            ++si;
        }
    } else {
        for (const sema::Symbol* sym : storage) {
            Component comp;
            comp.storageIdx = si;
            comp.declName   = std::string(sym->name);
            comp.isSkip     = false;
            const ast::PropertyDecl* pd =
                dynamic_cast<const ast::PropertyDecl*>(sym->decl);
            if (pd && pd->serialized) {
                comp.isSkip  = pd->serialized->isSkip;
                comp.wireName = pd->serialized->name.empty()
                    ? comp.declName : std::string(pd->serialized->name);
            } else {
                comp.wireName = comp.declName;
            }
            comp.ty = recordMemberTypeResolved(sym);
            components.push_back(std::move(comp));
            ++si;
        }
    }

    // Allocate local slots for each component (using the component's LLVM type).
    llvm::StructType* st = recordStruct(ts);
    // Accumulate field values in an alloca of the full struct.
    llvm::AllocaInst* resultSlot = entryAlloca(st);
    builder_.CreateStore(llvm::ConstantAggregateZero::get(st), resultSlot);

    // For each non-skip component, allocate a scratch slot initialized to default.
    std::vector<llvm::AllocaInst*> slots(components.size(), nullptr);
    for (unsigned i = 0; i < components.size(); ++i) {
        if (components[i].isSkip) continue;
        if (i >= st->getNumElements()) continue;
        llvm::Type* et = st->getElementType(i);
        llvm::AllocaInst* slot = entryAlloca(et);
        builder_.CreateStore(llvm::Constant::getNullValue(et), slot);
        slots[i] = slot;
    }

    // BeginObject → read field count.
    auto* beginFn = declareC("bz_binder_begin_object", i32(), {ptrTy()});
    llvm::Value* fieldCount = builder_.CreateCall(beginFn, {derPtr}, "deser.count");

    // Loop: iterate fieldCount times, reading name + dispatching.
    auto* loopHdr  = llvm::BasicBlock::Create(llvm_, "deser.loop.hdr",  currentFn_);
    auto* loopBody = llvm::BasicBlock::Create(llvm_, "deser.loop.body", currentFn_);
    auto* loopEnd  = llvm::BasicBlock::Create(llvm_, "deser.loop.end",  currentFn_);

    // Loop counter alloca.
    llvm::AllocaInst* idxSlot = entryAlloca(i32());
    builder_.CreateStore(builder_.getInt32(0), idxSlot);
    builder_.CreateBr(loopHdr);

    // Loop header: idx < fieldCount?
    builder_.SetInsertPoint(loopHdr);
    llvm::Value* idx = builder_.CreateLoad(i32(), idxSlot, "deser.idx");
    llvm::Value* cond = builder_.CreateICmpSLT(idx, fieldCount, "deser.more");
    builder_.CreateCondBr(cond, loopBody, loopEnd);

    // Loop body: read field name, dispatch, increment.
    builder_.SetInsertPoint(loopBody);

    auto* readNameFn = declareC("bz_binder_read_field_name", ptrTy(), {ptrTy()});
    llvm::Value* namePtr = builder_.CreateCall(readNameFn, {derPtr}, "deser.fname");

    // Dispatch: compare namePtr against each component's wire name.
    // Use bz_string_compare (runtime ordinal comparison) to test equality.
    // We need the wire-name string lengths too for the C comparison function:
    //   bz_string_compare(a_data, a_len, b_data, b_len) → i32
    auto* cmpFn = declareC("bz_string_compare", i32(),
                           {ptrTy(), i64(), ptrTy(), i64(), i32()});

    // Build dispatch blocks.
    auto* skipBB = llvm::BasicBlock::Create(llvm_, "deser.skip", currentFn_);
    auto* afterBB = llvm::BasicBlock::Create(llvm_, "deser.after", currentFn_);

    for (unsigned ci = 0; ci < components.size(); ++ci) {
        const Component& comp = components[ci];
        if (comp.isSkip) continue;
        if (!slots[ci]) continue;

        const std::string& wn = comp.wireName;

        // Get the data pointer and length of the wire name (a bz_string*).
        // We have a namePtr (bz_string*) and want to compare it to the
        // literal wire name. Build a constant string for the wire name.
        llvm::Value* litStr = lowerStringLiteral(
            std::string("\"") + wn + "\"", false);

        // For the comparison: bz_string_compare needs (data, len) for both sides.
        // Extract data (field 0) and len (field 1) from both namePtr and litStr.
        // bz_string layout (stringType()): {ptr data, i64 len, i8 width, i64 rc}.
        auto* strTy = stringType();
        llvm::Value* litData = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(strTy, litStr, 0), "lit.data");
        llvm::Value* litLen  = builder_.CreateLoad(i64(),
            builder_.CreateStructGEP(strTy, litStr, 1), "lit.len");
        llvm::Value* nameData = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(strTy, namePtr, 0), "name.data");
        llvm::Value* nameLen  = builder_.CreateLoad(i64(),
            builder_.CreateStructGEP(strTy, namePtr, 1), "name.len");

        llvm::Value* cmpRes = builder_.CreateCall(cmpFn,
            {nameData, nameLen, litData, litLen, builder_.getInt32(0)}, "deser.cmp");
        llvm::Value* isMatch = builder_.CreateICmpEQ(
            cmpRes, builder_.getInt32(0), "deser.match");

        auto* readBB = llvm::BasicBlock::Create(llvm_, "deser.rd", currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "deser.nd", currentFn_);
        builder_.CreateCondBr(isMatch, readBB, nextBB);

        builder_.SetInsertPoint(readBB);
        // Read the field value into its slot.
        llvm::Value* fieldVal = emitDeserializeField(comp.ty, derPtr);
        if (fieldVal && slots[ci]) {
            llvm::Type* et = st->getElementType(ci);
            if (fieldVal->getType() == et)
                builder_.CreateStore(fieldVal, slots[ci]);
        }
        // Release namePtr (bz_string_release).
        auto* relFn = declareC("bz_string_release", voidTy, {ptrTy()});
        builder_.CreateCall(relFn, {namePtr});
        builder_.CreateBr(afterBB);

        builder_.SetInsertPoint(nextBB);
    }

    // No component matched → SkipValue (unknown field policy delegated to deserializer).
    builder_.CreateBr(skipBB);
    builder_.SetInsertPoint(skipBB);
    auto* skipFn = declareC("bz_binder_skip_value", voidTy, {ptrTy()});
    builder_.CreateCall(skipFn, {derPtr});
    // Release namePtr.
    auto* relFn2 = declareC("bz_string_release", voidTy, {ptrTy()});
    builder_.CreateCall(relFn2, {namePtr});
    builder_.CreateBr(afterBB);

    builder_.SetInsertPoint(afterBB);
    // Increment idx.
    llvm::Value* newIdx = builder_.CreateAdd(idx, builder_.getInt32(1), "deser.idx1");
    builder_.CreateStore(newIdx, idxSlot);
    builder_.CreateBr(loopHdr);

    // Loop end.
    builder_.SetInsertPoint(loopEnd);

    // EndObject.
    auto* endFn = declareC("bz_binder_end_object", voidTy, {ptrTy()});
    builder_.CreateCall(endFn, {derPtr});

    // Build result struct: insert all non-skip fields from their slots.
    llvm::Value* result = builder_.CreateLoad(st, resultSlot, "deser.rec");
    for (unsigned ci = 0; ci < components.size(); ++ci) {
        if (components[ci].isSkip) continue;
        if (!slots[ci]) continue;
        if (ci >= st->getNumElements()) continue;
        llvm::Value* fv = builder_.CreateLoad(st->getElementType(ci), slots[ci]);
        result = builder_.CreateInsertValue(result, fv, {ci});
    }

    return result;
}

// ---------------------------------------------------------------------------
// Emitter::emitDeserializeField
//
// Emit the read of a single field of Bezel type `fieldTy` from `derPtr`.
// Returns the read value (or a zero/null default on unknown type).
// ---------------------------------------------------------------------------
llvm::Value* Emitter::emitDeserializeField(const sema::TypeRef* fieldTy,
                                           llvm::Value* derPtr) {
    using K = sema::TypeRef::Kind;

    if (!fieldTy) return nullptr;

    // §9.8 / #638 / #637: nullable check MUST come before Primitive/Named dispatch.
    // For int?/bool? the kind is Primitive but the wire starts with a null marker;
    // for string? the kind is also Primitive — reaching the Primitive branch without
    // the null check means bz_binder_read_string is called unconditionally, returning
    // "" when the marker indicates null (#637).  Non-nullable value types return an
    // inner TypeRef with nullable=false which then re-dispatches correctly.
    if (fieldTy->nullable) {
        // Read null marker or inner value.
        auto* tryNullFn = declareC("bz_binder_try_read_null", i8(), {ptrTy()});
        llvm::Value* wasNull = builder_.CreateCall(tryNullFn, {derPtr}, "deser.wnull");
        llvm::Value* isNull  = builder_.CreateICmpNE(
            wasNull, builder_.getInt8(0), "deser.isnull");

        auto* nullBB = llvm::BasicBlock::Create(llvm_, "deser.nullfld", currentFn_);
        auto* liveBB = llvm::BasicBlock::Create(llvm_, "deser.livefld", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "deser.nend",    currentFn_);
        builder_.CreateCondBr(isNull, nullBB, liveBB);

        // null path: return a null/zero value for the nullable type.
        builder_.SetInsertPoint(nullBB);
        llvm::Value* nullVal = nullptr;
        if (fieldTy->kind == K::Primitive &&
            fieldTy->primitive == sema::PrimitiveType::String) {
            nullVal = llvm::ConstantPointerNull::get(ptrTy());
        } else {
            // nullable value: return the full nullable-value struct zeroed.
            llvm::Type* thisLlvmTy = lowerTypeRef(fieldTy);
            nullVal = thisLlvmTy ? llvm::Constant::getNullValue(thisLlvmTy) : nullptr;
        }
        builder_.CreateBr(endBB);
        auto* nullEndBB = builder_.GetInsertBlock();

        // live path: read the inner (non-nullable) value.
        builder_.SetInsertPoint(liveBB);
        sema::TypeRef nonNullTy = *fieldTy;
        nonNullTy.nullable = false;
        llvm::Value* innerVal = emitDeserializeField(&nonNullTy, derPtr);
        // For nullable VALUE types ({i8 hasValue, T value}), the raw inner read
        // returns the inner T.  Wrap it back into the tagged struct so the PHI
        // and the record slot both see {i8=1, T=value}.  Reference-nullable
        // (string?, class?) stays as a plain ptr — no wrap needed (#638).
        if (innerVal && isNullableValueTy(fieldTy))
            innerVal = wrapNullableValue(fieldTy, innerVal);
        builder_.CreateBr(endBB);
        auto* liveEndBB = builder_.GetInsertBlock();

        builder_.SetInsertPoint(endBB);
        if (nullVal && innerVal && nullVal->getType() == innerVal->getType()) {
            llvm::PHINode* phi = builder_.CreatePHI(nullVal->getType(), 2, "deser.nval");
            phi->addIncoming(nullVal, nullEndBB);
            phi->addIncoming(innerVal, liveEndBB);
            return phi;
        }
        return innerVal ? innerVal : nullVal;
    }

    if (fieldTy->kind == K::Primitive) {
        switch (fieldTy->primitive) {
        case sema::PrimitiveType::Bool: {
            auto* fn = declareC("bz_binder_read_bool", i8(), {ptrTy()});
            llvm::Value* b8 = builder_.CreateCall(fn, {derPtr}, "deser.bool");
            return builder_.CreateTrunc(b8, i1(), "deser.bool1");
        }
        case sema::PrimitiveType::Char: {
            auto* fn = declareC("bz_binder_read_char", i32(), {ptrTy()});
            return builder_.CreateCall(fn, {derPtr}, "deser.char");
        }
        case sema::PrimitiveType::Byte: {
            auto* fn = declareC("bz_binder_read_uint", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.u64");
            return builder_.CreateTrunc(v, i8(), "deser.byte");
        }
        case sema::PrimitiveType::SByte: {
            auto* fn = declareC("bz_binder_read_int", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.i64");
            return builder_.CreateTrunc(v, i8(), "deser.sbyte");
        }
        case sema::PrimitiveType::Short: {
            auto* fn = declareC("bz_binder_read_int", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.i64");
            return builder_.CreateTrunc(v, i16(), "deser.short");
        }
        case sema::PrimitiveType::UShort: {
            auto* fn = declareC("bz_binder_read_uint", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.u64");
            return builder_.CreateTrunc(v, i16(), "deser.ushort");
        }
        case sema::PrimitiveType::Int: {
            auto* fn = declareC("bz_binder_read_int", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.i64");
            return builder_.CreateTrunc(v, i32(), "deser.int");
        }
        case sema::PrimitiveType::UInt: {
            auto* fn = declareC("bz_binder_read_uint", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.u64");
            return builder_.CreateTrunc(v, i32(), "deser.uint");
        }
        case sema::PrimitiveType::Long: {
            auto* fn = declareC("bz_binder_read_int", i64(), {ptrTy()});
            return builder_.CreateCall(fn, {derPtr}, "deser.long");
        }
        case sema::PrimitiveType::ULong: {
            auto* fn = declareC("bz_binder_read_uint", i64(), {ptrTy()});
            return builder_.CreateCall(fn, {derPtr}, "deser.ulong");
        }
        case sema::PrimitiveType::Float: {
            auto* fn = declareC("bz_binder_read_float",
                                llvm::Type::getDoubleTy(llvm_), {ptrTy()});
            llvm::Value* d = builder_.CreateCall(fn, {derPtr}, "deser.dbl");
            return builder_.CreateFPTrunc(d, llvm::Type::getFloatTy(llvm_), "deser.f");
        }
        case sema::PrimitiveType::Double: {
            auto* fn = declareC("bz_binder_read_float",
                                llvm::Type::getDoubleTy(llvm_), {ptrTy()});
            return builder_.CreateCall(fn, {derPtr}, "deser.dbl");
        }
        case sema::PrimitiveType::Decimal: {
            auto* fn = declareC("bz_binder_read_float",
                                llvm::Type::getDoubleTy(llvm_), {ptrTy()});
            return builder_.CreateCall(fn, {derPtr}, "deser.dec");
        }
        case sema::PrimitiveType::String: {
            auto* fn = declareC("bz_binder_read_string", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {derPtr}, "deser.str");
        }
        default:
            return nullptr;
        }
    }

    if (fieldTy->kind == K::Named && fieldTy->declaration) {
        const TypeSymbol* fieldTs = fieldTy->declaration;

        if (fieldTs->kind == ast::TypeDeclKind::Enum)
            return emitEnumDeserialize(fieldTs, derPtr);

        if (fieldTs->kind == ast::TypeDeclKind::Record)
            return emitRecordDeserialize(fieldTs, derPtr);
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Emitter::emitEnumDeserialize  §9.8
//
// Reads a string from the deserializer and maps it to a declared enum constant.
// Returns an i32 value.  Raises SerializationException if no match.
// ---------------------------------------------------------------------------
llvm::Value* Emitter::emitEnumDeserialize(const TypeSymbol* ts,
                                          llvm::Value* derPtr) {
    if (!ts || !ts->decl) return builder_.getInt32(0);
    const auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    if (!td) return builder_.getInt32(0);
    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);

    // Read the string.
    auto* readFn = declareC("bz_binder_read_string", ptrTy(), {ptrTy()});
    llvm::Value* namePtr = builder_.CreateCall(readFn, {derPtr}, "deser.ename");

    // Alloca for the result enum value.
    llvm::AllocaInst* resultSlot = entryAlloca(i32());
    builder_.CreateStore(builder_.getInt32(0), resultSlot);

    auto* endBB   = llvm::BasicBlock::Create(llvm_, "enum.deser.end",   currentFn_);
    auto* throwBB = llvm::BasicBlock::Create(llvm_, "enum.deser.throw", currentFn_);

    auto* cmpFn = declareC("bz_string_compare", i32(),
                           {ptrTy(), i64(), ptrTy(), i64(), i32()});

    for (const auto& ec : td->enumConstants) {
        int32_t ordinal = -1;
        for (const Symbol* m : ts->members) {
            if (m->name == ec.name) {
                if (dynamic_cast<const EnumConstantSymbol*>(m)) {
                    auto it = enumValue_.find(m);
                    if (it != enumValue_.end()) ordinal = (int32_t)it->second;
                }
                break;
            }
        }
        if (ordinal < 0) continue;

        std::string wireName = std::string(enumConstWireName(ec));
        llvm::Value* litStr  = lowerStringLiteral(
            std::string("\"") + wireName + "\"", false);

        // Extract data/len from both strings.
        // bz_string layout (stringType()): {ptr data, i64 len, i8 width, i64 rc}.
        auto* strTy2 = stringType();
        llvm::Value* litData  = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(strTy2, litStr, 0), "lit.data");
        llvm::Value* litLen   = builder_.CreateLoad(i64(),
            builder_.CreateStructGEP(strTy2, litStr, 1), "lit.len");
        llvm::Value* nameData = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(strTy2, namePtr, 0), "name.data");
        llvm::Value* nameLen  = builder_.CreateLoad(i64(),
            builder_.CreateStructGEP(strTy2, namePtr, 1), "name.len");

        llvm::Value* cmpRes  = builder_.CreateCall(cmpFn,
            {nameData, nameLen, litData, litLen, builder_.getInt32(0)}, "enum.cmp");
        llvm::Value* isMatch = builder_.CreateICmpEQ(
            cmpRes, builder_.getInt32(0), "enum.match");

        auto* matchBB = llvm::BasicBlock::Create(llvm_, "enum.deser.match", currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "enum.deser.next",  currentFn_);
        builder_.CreateCondBr(isMatch, matchBB, nextBB);

        builder_.SetInsertPoint(matchBB);
        builder_.CreateStore(builder_.getInt32(ordinal), resultSlot);
        // Release namePtr.
        auto* relFn = declareC("bz_string_release", voidTy, {ptrTy()});
        builder_.CreateCall(relFn, {namePtr});
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(nextBB);
    }

    // No match → throw.
    builder_.CreateBr(throwBB);

    builder_.SetInsertPoint(throwBB);
    {
        auto* relFn = declareC("bz_string_release", voidTy, {ptrTy()});
        builder_.CreateCall(relFn, {namePtr});
        auto* throwFn = declareC("bz_ser_throw", voidTy, {ptrTy(), ptrTy()});
        llvm::Value* msg = builder_.CreateGlobalString(
            "Enum constant not found during deserialization (SS9.8)",
            "enum.exc.msg");
        builder_.CreateCall(throwFn, {msg,
            llvm::ConstantPointerNull::get(ptrTy())});
    }
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    return builder_.CreateLoad(i32(), resultSlot, "enum.deser.v");
}

// ---------------------------------------------------------------------------
// Emitter::isSerializationCall
//
// Returns true iff the call is a serialization-related call that should be
// handled by tryLowerSerializationCall.  This is a lightweight pre-check that
// avoids complex dispatch in lowerCall's hot path.
// ---------------------------------------------------------------------------
bool Emitter::isSerializationCall(const ast::CallExpr& call,
                                  const ast::MemberAccessExpr& ma) {
    const std::string_view mn = ma.member;
    if (!ma.target) return false;

    // BinarySerializer / BinaryDeserializer instance methods.
    auto* recvTy = ctx_.exprType(*ma.target);
    if (recvTy && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty()) {
        const std::string_view rcn = recvTy->astNode->name->parts[0];
        if (rcn == "BinarySerializer" || rcn == "BinaryDeserializer") return true;
        // OwnedBuffer<byte>.AsReadOnlySpan() / AsSpan().
        if ((rcn == "OwnedBuffer" || rcn == "Array") &&
            (mn == "AsReadOnlySpan" || mn == "AsSpan")) {
            bool isByte = !recvTy->genericArgs.empty() &&
                recvTy->genericArgs[0] &&
                recvTy->genericArgs[0]->kind == sema::TypeRef::Kind::Primitive &&
                recvTy->genericArgs[0]->primitive == sema::PrimitiveType::Byte;
            if (isByte) return true;
        }
    }

    // Synthesized Serialize on record/enum.
    if (mn == "Serialize" && call.args.size() == 1) {
        auto* recvTy2 = ctx_.exprType(*ma.target);
        if (recvTy2 && recvTy2->declaration) {
            const TypeSymbol* ts = recvTy2->declaration;
            if ((ts->kind == ast::TypeDeclKind::Record ||
                 ts->kind == ast::TypeDeclKind::Enum) &&
                declaresConformance(ts, "ISerializable") &&
                !hasHandWrittenMethod(ts, "Serialize"))
                return true;
        }
    }

    // Synthesized static Deserialize on record/enum type name.
    if (mn == "Deserialize" && call.args.size() == 1) {
        // Instance Deserialize on record (rare but possible if hand-written?)
        // Check for static: target resolves to a TypeSymbol.
        const sema::Symbol* sym = ctx_.exprSymbol(*ma.target);
        if (auto* ts = dynamic_cast<const TypeSymbol*>(sym)) {
            if ((ts->kind == ast::TypeDeclKind::Record ||
                 ts->kind == ast::TypeDeclKind::Enum) &&
                declaresConformance(ts, "IDeserializable") &&
                !hasHandWrittenMethod(ts, "Deserialize"))
                return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Emitter::tryLowerSerializationCall  §9.8 / §2.15
//
// Intercept call expressions related to BinarySerializer/BinaryDeserializer
// and the synthesized Serialize/Deserialize methods.
//
// Returns lowered value or nullptr if not a serialization call.
// ---------------------------------------------------------------------------
llvm::Value* Emitter::tryLowerSerializationCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    const std::string_view methodName = ma->member;

    // -----------------------------------------------------------------------
    // BinarySerializer instance methods: ToBytes(), ISerializer methods,
    // Dispose.  Receiver type name is "BinarySerializer".
    // -----------------------------------------------------------------------
    auto* recvTy = ctx_.exprType(*ma->target);
    if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
        !recvTy->declaration &&
        recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty()) {
        const std::string_view rcn = recvTy->astNode->name->parts[0];

        if (rcn == "BinarySerializer") {
            llvm::Value* ser = lowerExpr(*ma->target);
            if (!ser) return nullptr;

            // ToBytes() → bz_binser_to_bytes(ser) returning bz_array_t*
            if (methodName == "ToBytes" && call.args.empty()) {
                auto* fn = declareC("bz_binser_to_bytes", ptrTy(), {ptrTy()});
                return builder_.CreateCall(fn, {ser}, "binser.bytes");
            }

            // Dispose() → bz_binser_free(ser)
            if (methodName == "Dispose" && call.args.empty()) {
                auto* fn = declareC("bz_binser_free",
                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {ser});
                return nullptr;  // void
            }
            return nullptr;
        }

        if (rcn == "BinaryDeserializer") {
            llvm::Value* der = lowerExpr(*ma->target);
            if (!der) return nullptr;

            // Dispose() → bz_binder_free(der)
            if (methodName == "Dispose" && call.args.empty()) {
                auto* fn = declareC("bz_binder_free",
                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {der});
                return nullptr;
            }
            return nullptr;
        }

        // OwnedBuffer<byte>.AsReadOnlySpan() → span {ptr, 0, count}
        if (rcn == "OwnedBuffer" || rcn == "Array") {
            bool isOwnedBuffer = (rcn == "OwnedBuffer");
            // Only intercept .AsReadOnlySpan() and .AsSpan() on byte buffers.
            bool isByteBuffer = !recvTy->genericArgs.empty() &&
                recvTy->genericArgs[0] &&
                recvTy->genericArgs[0]->kind == sema::TypeRef::Kind::Primitive &&
                recvTy->genericArgs[0]->primitive == sema::PrimitiveType::Byte;
            if (isByteBuffer &&
                (methodName == "AsReadOnlySpan" || methodName == "AsSpan") &&
                call.args.empty()) {
                llvm::Value* arr = lowerExpr(*ma->target);
                if (!arr) return nullptr;
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* count64 = builder_.CreateCall(lenFn, {arr}, "ob.count");
                llvm::Value* count   = builder_.CreateTrunc(count64, i32(), "ob.cnt32");
                return buildArraySpan(arr, builder_.getInt32(0), count);
            }
            (void)isOwnedBuffer;
        }
    }

    // -----------------------------------------------------------------------
    // Synthesized Serialize(s: ISerializer) on a record or enum receiver.
    // -----------------------------------------------------------------------
    if (methodName == "Serialize" && call.args.size() == 1 && call.args[0].value) {
        auto* recvTy2 = ctx_.exprType(*ma->target);
        if (recvTy2 && recvTy2->kind == sema::TypeRef::Kind::Named &&
            recvTy2->declaration) {
            const TypeSymbol* ts = recvTy2->declaration;
            bool isRecord = ts->kind == ast::TypeDeclKind::Record;
            bool isEnum   = ts->kind == ast::TypeDeclKind::Enum;
            if ((isRecord || isEnum) &&
                declaresConformance(ts, "ISerializable") &&
                !hasHandWrittenMethod(ts, "Serialize")) {
                llvm::Value* serArg = lowerExpr(*call.args[0].value);
                if (!serArg) return nullptr;
                llvm::Value* recv = lowerExpr(*ma->target);
                if (!recv) return nullptr;

                if (isRecord) {
                    llvm::StructType* st = recordStruct(ts);
                    if (recv->getType()->isPointerTy())
                        recv = builder_.CreateLoad(st, recv);
                    emitRecordSerialize(ts, recv, serArg);
                } else {
                    // Enum: recv is i32.
                    if (!recv->getType()->isIntegerTy(32))
                        recv = builder_.CreateSExtOrTrunc(recv, i32(), "enum.v");
                    emitEnumSerialize(ts, recv, serArg);
                }
                return nullptr;  // void method
            }
        }
    }

    // -----------------------------------------------------------------------
    // Synthesized static Deserialize(d: IDeserializer) on a record or enum type.
    // The callee is a MemberAccessExpr with target = IdentifierExpr(<TypeName>).
    // -----------------------------------------------------------------------
    if (methodName == "Deserialize" && call.args.size() == 1 && call.args[0].value) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (id) {
            // Resolve the identifier to a TypeSymbol.
            const sema::Symbol* sym = ctx_.exprSymbol(*ma->target);
            const TypeSymbol* ts = dynamic_cast<const TypeSymbol*>(sym);
            if (ts && (ts->kind == ast::TypeDeclKind::Record ||
                       ts->kind == ast::TypeDeclKind::Enum) &&
                declaresConformance(ts, "IDeserializable") &&
                !hasHandWrittenMethod(ts, "Deserialize")) {
                llvm::Value* derArg = lowerExpr(*call.args[0].value);
                if (!derArg) return nullptr;

                if (ts->kind == ast::TypeDeclKind::Record)
                    return emitRecordDeserialize(ts, derArg);
                else
                    return emitEnumDeserialize(ts, derArg);
            }
        }
    }

    return nullptr;
}

} // namespace bezel::codegen
