// LlvmBackend_Serialization.cpp — §9.8 / §2.14–2.15 synthesis codegen.
//
// Tier 1: records and enums (tree shape, Binary format only).
// Tier 2: classes with `serialized tree` clause (§9.8 v0.136).
//         `serialized graph`, `serialized cyclic`, and `polymorphic` emit
//         a clean tier-boundary diagnostic (not yet implemented).
//
// BinarySerializer / BinaryDeserializer construction, ToBytes, AsReadOnlySpan,
// and Dispose forwarding.
//
// Spec: lang §9.8; stdlib §2.14 (ISerializer/IDeserializer), §2.15
// (BinarySerializer/BinaryDeserializer), §4.14 (OwnedBuffer<T>/Span<T>).
// Issue #550, tiers 1+2.

#include "codegen/LlvmBackend_impl.hpp"

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace bezel::codegen {

using namespace bezel::sema;
using namespace bezel::ast;

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

// Case-fold a string to lowercase (ASCII only) for constructor-parameter
// name matching per §9.8 v0.136.  The idiomatic Bezel convention uses
// PascalCase for field/property declared identifiers and camelCase for
// constructor parameters; the synthesized-Deserialize constructor-selection
// match treats these as equivalent by comparing lowercased forms.
static std::string toLowerAscii(std::string_view sv) {
    std::string r(sv);
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

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
// Class serialization helpers (§9.8 tier 2, v0.136)
// ---------------------------------------------------------------------------

// Describes one serialized component of a class.
struct ClassComponent {
    std::string wireName;    // wire name (serialized "x" override or declared name)
    std::string declName;    // declared identifier (matches ctor param name)
    const sema::TypeRef* ty; // resolved component type
    unsigned fieldIdx;       // index in classStruct (for GEP on serialize)
    bool isSkip     = false; // serialized skip
    bool isOptional = false; // serialized optional
    bool isOwned    = false; // `owned` class-typed component (not a value field)
    bool isBackEdge = false; // §9.8 tier 3: non-owned class-typed back-edge (graph/cyclic)
};

// Collect the serialized components of a class in declaration order.
// Per §9.8: owned and value-typed fields/auto-properties, PLUS back-edge
// (non-owned class-typed) fields when the class is `serialized graph/cyclic`.
// The `isBackEdge` flag on each component distinguishes the two categories.
// Under `serialized tree`, back-edges are excluded (they violate the tree assertion).
//
// `storage` must already be populated by collectInstanceStorage().
// `fieldIndex_` must be current (call classStruct(ts) first).
// `includeBackEdges`: include non-owned class-typed fields as back-edge
// components (true for `serialized graph/cyclic`; false for `serialized tree`).
static void collectClassComponents(
        const TypeSymbol* ts,
        const std::vector<const Symbol*>& storage,
        const std::unordered_map<const Symbol*, unsigned>& fieldIndex,
        sema::SemaContext& ctx,
        std::vector<ClassComponent>& out,
        bool includeBackEdges = false) {
    if (!ts || !ts->decl) return;
    const auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    if (!td) return;

    for (const Symbol* m : storage) {
        // Find the field index.
        auto idxIt = fieldIndex.find(m);
        if (idxIt == fieldIndex.end()) continue;

        ClassComponent comp;
        comp.fieldIdx = idxIt->second;
        comp.declName = std::string(m->name);
        comp.ty       = nullptr;

        // Determine ty, wireName, skip, optional, isOwned from the symbol's decl.
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl)) {
            // Check if owned modifier is set.
            bool isOwned = false;
            for (auto mod : fd->modifiers) if (mod == "owned") { isOwned = true; break; }
            comp.isOwned = isOwned;
            comp.wireName = comp.declName;
            comp.ty = ctx.resolvedType(*fd->type);

            // §9.8 tier 3: identify back-edges (non-owned class-typed fields).
            if (!isOwned && comp.ty) {
                bool fieldIsClass =
                    (comp.ty->kind == sema::TypeRef::Kind::Named &&
                     comp.ty->declaration &&
                     comp.ty->declaration->kind == ast::TypeDeclKind::Class);
                bool fieldIsNullableClass = false;
                if (!fieldIsClass && comp.ty->nullable) {
                    fieldIsNullableClass =
                        comp.ty->kind == sema::TypeRef::Kind::Named &&
                        comp.ty->declaration &&
                        comp.ty->declaration->kind == ast::TypeDeclKind::Class;
                }
                if (fieldIsClass || fieldIsNullableClass) {
                    if (!includeBackEdges) continue;  // skip for tree mode
                    comp.isBackEdge = true;
                }
            }
        } else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl)) {
            if (pd->serialized) {
                comp.isSkip     = pd->serialized->isSkip;
                comp.isOptional = pd->serialized->isOptional;
                if (!pd->serialized->name.empty())
                    comp.wireName = std::string(pd->serialized->name);
                else
                    comp.wireName = comp.declName;
            } else {
                comp.wireName = comp.declName;
            }
            if (pd->type) comp.ty = ctx.resolvedType(*pd->type);
        } else {
            // Fallback (no decl — e.g. imported field).
            comp.wireName = comp.declName;
            comp.ty = ctx.importedMemberType(m);
        }

        if (comp.isSkip) { out.push_back(std::move(comp)); continue; }
        if (!comp.ty) continue;

        out.push_back(std::move(comp));
    }
}

// §9.8 v0.141 / #703: is this component type a builtin Shared<T> or Weak<T>?
// These are admitted to serialization eligibility by sema slice S1, but their
// type-driven wire codegen (Shared<T> → identity $id/$ref; Weak<T> → back-
// reference $ref) lands in a later slice. Shared/Weak are builtin handles with
// no class declaration, so emitting an owning-component body for them would
// miscompile; emitClassSerialize/Deserialize guard on this and report a clean
// "not yet implemented" error instead. The name lives in astNode (locally) or
// importedName (cross-module metadata).
static bool isSharedOrWeakComponentType(const sema::TypeRef* tr) {
    if (!tr) return false;
    const sema::TypeRef* t = tr;
    sema::TypeRef peeled;
    if (t->nullable) { peeled = *t; peeled.nullable = false; t = &peeled; }
    if (t->kind != sema::TypeRef::Kind::Named || t->declaration) return false;
    std::string_view name;
    if (t->astNode && t->astNode->name && !t->astNode->name->parts.empty())
        name = t->astNode->name->parts[0];
    else if (!t->importedName.empty())
        name = t->importedName;
    return name == "Shared" || name == "Weak";
}

// §9.8 v0.141 / #703 slice S2: if `tr` is a Shared<C> component that the
// type-driven wire codegen HANDLES — a non-nullable Shared whose element C is a
// serialization-eligible CLASS (so emitClassSerialize/Deserialize can write/read
// its inline body) — return C's TypeSymbol; otherwise nullptr. Shared<C> is the
// identity participant of the type-driven model: its tag keys on the underlying
// value (bz_shared_get) on serialize, and on deserialize the tag table holds the
// constructed HANDLE so aliases share one control block (bz_shared_clone).
// A nullable Shared<C>? is handled too (a null handle writes/reads the null
// marker; see the fty->nullable gating in the serialize/deserialize branches).
// Cases NOT yet handled (return nullptr → still caught by the codegen guard):
// Weak<T> (handled separately), Shared<non-class>, Shared<class-without-clause>.
static const TypeSymbol* handledSharedElementClass(const sema::TypeRef* tr) {
    if (!tr) return nullptr;
    const sema::TypeRef* t = tr;
    sema::TypeRef peeled;
    if (t->nullable) { peeled = *t; peeled.nullable = false; t = &peeled; }
    if (t->kind != sema::TypeRef::Kind::Named || t->declaration) return nullptr;
    std::string_view name;
    if (t->astNode && t->astNode->name && !t->astNode->name->parts.empty())
        name = t->astNode->name->parts[0];
    else if (!t->importedName.empty())
        name = t->importedName;
    if (name != "Shared") return nullptr;
    if (t->genericArgs.empty() || !t->genericArgs[0]) return nullptr;
    const sema::TypeRef* el = t->genericArgs[0];
    if (el->kind != sema::TypeRef::Kind::Named || !el->declaration) return nullptr;
    if (el->declaration->kind != ast::TypeDeclKind::Class) return nullptr;
    const auto* td = dynamic_cast<const ast::TypeDecl*>(el->declaration->decl);
    if (!td || !td->serializedClass) return nullptr;  // element must be synthesizable
    return el->declaration;
}

// §9.8 v0.141 / #703 slice S3: if `tr` is a Weak<C> component whose element C is
// a serialization-eligible class, return C's TypeSymbol; else nullptr. Weak<C>
// is a back-reference: it serializes as a $ref to the tag of the Shared<C>
// participant that owns C, and on deserialize it bz_shared_downgrade's that
// participant's handle. Nullable Weak<C>? is allowed (a null weak writes null).
// The FIELD must additionally be `owned` for the weak count to be released on
// disposal (checked at the call site via comp.isOwned).
static const TypeSymbol* handledWeakElementClass(const sema::TypeRef* tr) {
    if (!tr) return nullptr;
    const sema::TypeRef* t = tr;
    sema::TypeRef peeled;
    if (t->nullable) { peeled = *t; peeled.nullable = false; t = &peeled; }
    if (t->kind != sema::TypeRef::Kind::Named || t->declaration) return nullptr;
    std::string_view name;
    if (t->astNode && t->astNode->name && !t->astNode->name->parts.empty())
        name = t->astNode->name->parts[0];
    else if (!t->importedName.empty())
        name = t->importedName;
    if (name != "Weak") return nullptr;
    if (t->genericArgs.empty() || !t->genericArgs[0]) return nullptr;
    const sema::TypeRef* el = t->genericArgs[0];
    if (el->kind != sema::TypeRef::Kind::Named || !el->declaration) return nullptr;
    if (el->declaration->kind != ast::TypeDeclKind::Class) return nullptr;
    const auto* td = dynamic_cast<const ast::TypeDecl*>(el->declaration->decl);
    if (!td || !td->serializedClass) return nullptr;
    return el->declaration;
}

// §9.8 v0.141 / #703: is `comp` a Shared/Weak component whose type-driven wire
// codegen IS implemented (so the guard should NOT fire for it)? Shared<class>
// (non-nullable) and `owned` Weak<class> are handled; everything else Shared/Weak
// (nullable Shared?, Shared<non-class>, non-owned Weak, Weak<non-class>) is not.
static bool isHandledTypeDrivenComponent(const ClassComponent& comp,
                                         bool isGraphMode) {
    if (!isGraphMode) return false;
    if (handledSharedElementClass(comp.ty)) return true;
    if (comp.isOwned && handledWeakElementClass(comp.ty)) return true;
    return false;
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
    // §2.15 / fix #687: bz_binser_begin_object can now set bz_exc (MaxDepth guard).
    // Check for pending exception so a try/catch SerializationException can catch it.
    emitPendingCheck();

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
            // §2.14 / #665: WriteDecimal — pass the 16-byte {i32×4} struct by
            // pointer to bz_binser_write_decimal (wire format: 4×u32 LE).
            // Spill the struct value to an alloca so we can pass its address.
            llvm::AllocaInst* decSlot = entryAlloca(decimalStructType());
            builder_.CreateStore(val, decSlot);
            auto* fn = declareC("bz_binser_write_decimal", voidTy,
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {serPtr, decSlot});
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

        if (fieldTs->kind == ast::TypeDeclKind::Class) {
            // §9.8 tier 2/tier 4: owned class-typed component.
            // `val` is a ptr to the class instance.
            auto* td = dynamic_cast<const ast::TypeDecl*>(fieldTs->decl);
            if (td && td->serializedClass) {
                // §9.8 tier 4: if the class has a polymorphic registry, use
                // polymorphic dispatch (emitPolyClassSerialize) which reads the
                // dynamic type from the vtable and writes "$type" + concrete body.
                if (!td->serializedClass->polyCases.empty())
                    emitPolyClassSerialize(fieldTs, val, serPtr);
                else
                    emitClassSerialize(fieldTs, val, serPtr);
                return;
            }
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
        bool isOptional = false; // `serialized optional`: missing → default(T)
        unsigned storageIdx;    // index into the struct (ExtractValue / InsertValue)
    };
    std::vector<Component> components;
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(ts, storage);

    unsigned si = 0;
    if (td->hasPositionalFields) {
        for (const auto& pf : td->positionalFields) {
            Component comp;
            comp.storageIdx  = si;
            comp.declName    = std::string(pf.name);
            comp.isSkip      = positionalFieldIsSkip(pf);
            comp.isOptional  = pf.serialized && pf.serialized->isOptional;
            comp.wireName    = comp.isSkip ? "" : std::string(positionalFieldWireName(pf));
            comp.ty          = si < storage.size()
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
            comp.isOptional = false;
            const ast::PropertyDecl* pd =
                dynamic_cast<const ast::PropertyDecl*>(sym->decl);
            if (pd && pd->serialized) {
                comp.isSkip     = pd->serialized->isSkip;
                comp.isOptional = pd->serialized->isOptional;
                comp.wireName   = pd->serialized->name.empty()
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

    // fix #668: §9.8 / §2.15: allocate seen-flags for required components.
    // Required = non-skip AND non-optional. `serialized optional` exempts a
    // component; naked nullable T? (no optional clause) is still required.
    struct RecordSeenEntry {
        unsigned compIdx;
        std::string declName;
        llvm::AllocaInst* seenSlot;  // i8: 0=unseen, 1=seen
    };
    std::vector<RecordSeenEntry> recSeenFlags;
    for (unsigned i = 0; i < components.size(); ++i) {
        if (components[i].isSkip) continue;
        if (components[i].isOptional) continue;
        llvm::AllocaInst* sf = entryAlloca(i8());
        builder_.CreateStore(builder_.getInt8(0), sf);
        recSeenFlags.push_back({i, components[i].declName, sf});
    }

    // BeginObject → read field count.
    auto* beginFn = declareC("bz_binder_begin_object", i32(), {ptrTy()});
    llvm::Value* fieldCount = builder_.CreateCall(beginFn, {derPtr}, "deser.count");
    // §2.15: bz_binder_begin_object can throw (e.g. JSON: missing '{', truncated).
    emitPendingCheck();

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
    // §2.15: bz_binder_read_field_name can throw (JSON: malformed input).
    emitPendingCheck();

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
        // §2.15: bz_binder_read_* can throw (JSON: type mismatch, malformed).
        emitPendingCheck();
        // fix #668: mark this required component as seen.
        for (const auto& rse : recSeenFlags) {
            if (rse.compIdx == ci) {
                builder_.CreateStore(builder_.getInt8(1), rse.seenSlot);
                break;
            }
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
    // §2.15: bz_binder_skip_value can throw (JSON strict: unknown field).
    emitPendingCheck();
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
    // §2.15: bz_binder_end_object can throw (JSON: missing '}').
    emitPendingCheck();

    // fix #668: §9.8 / §2.15: check all required components were seen.
    // Records have no owned class fields so there is nothing to dispose on this
    // path — any already-read value-type slots (int, string, etc.) are held in
    // allocas that will simply be ignored if we throw.  String slots may hold a
    // bz_string* that needs release if it was already populated; for simplicity
    // we accept the minor leak on this rare structural-error path (matching the
    // behaviour of the truncated-input path for records which also does not
    // release string slots before emitThrowBuiltin).
    for (const auto& rse : recSeenFlags) {
        llvm::Value* seen = builder_.CreateLoad(i8(), rse.seenSlot, "rdeser.seen");
        llvm::Value* notSeen = builder_.CreateICmpEQ(seen, builder_.getInt8(0), "rdeser.nseen");
        auto* missBB = llvm::BasicBlock::Create(llvm_, "rdeser.miss", currentFn_);
        auto* okBB   = llvm::BasicBlock::Create(llvm_, "rdeser.ok",   currentFn_);
        builder_.CreateCondBr(notSeen, missBB, okBB);
        builder_.SetInsertPoint(missBB);
        emitThrowBuiltin("SerializationException");
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateUnreachable();
        builder_.SetInsertPoint(okBB);
    }

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
            // §2.14 / #665: ReadDecimal — bz_binder_read_decimal returns a
            // void* (ptr to alloca'd 16-byte struct), load it back to a value.
            auto* fn = declareC("bz_binder_read_decimal", ptrTy(), {ptrTy()});
            llvm::Value* decPtr = builder_.CreateCall(fn, {derPtr}, "deser.decptr");
            // decPtr is a ptr to a 16-byte {i32×4} struct on the runtime stack.
            // Load the decimal value via the pointer.
            llvm::AllocaInst* slot = entryAlloca(decimalStructType());
            {
                // Load from runtime-returned ptr only if non-null; otherwise
                // leave the alloca zero-initialised (default(decimal) = 0).
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    decPtr, llvm::ConstantPointerNull::get(ptrTy()), "dec.null");
                auto* loadBB = llvm::BasicBlock::Create(llvm_, "dec.load", currentFn_);
                auto* endBB  = llvm::BasicBlock::Create(llvm_, "dec.end",  currentFn_);
                builder_.CreateCondBr(isNull, endBB, loadBB);
                builder_.SetInsertPoint(loadBB);
                llvm::Value* decVal = builder_.CreateLoad(
                    decimalStructType(), decPtr, "deser.decv");
                builder_.CreateStore(decVal, slot);
                builder_.CreateBr(endBB);
                builder_.SetInsertPoint(endBB);
            }
            return builder_.CreateLoad(decimalStructType(), slot, "deser.dec");
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

        if (fieldTs->kind == ast::TypeDeclKind::Class) {
            // §9.8 tier 2/tier 4: deserialize a class-typed component.
            // Returns an owned ptr.
            auto* td = dynamic_cast<const ast::TypeDecl*>(fieldTs->decl);
            if (td && td->serializedClass) {
                // §9.8 tier 4: polymorphic base — dispatch via "$type" discriminator.
                if (!td->serializedClass->polyCases.empty())
                    return emitPolyClassDeserialize(fieldTs, derPtr);
                return emitClassDeserialize(fieldTs, derPtr);
            }
        }
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
// Emitter::emitPolyClassSerialize  §9.8 tier 4
//
// Polymorphic Serialize dispatch for a base class with a `polymorphic(...)` registry.
// `objPtr` is a ptr to the actual (concrete) class instance stored at the base
// class's static type.  Reads the vtable type-descriptor at vptr[0] to identify
// the concrete type, then writes "$type" + concrete fields for the matching case.
// An unregistered dynamic type raises SerializationException (§9.8: "closed set
// enforced on both directions").
// ---------------------------------------------------------------------------
void Emitter::emitPolyClassSerialize(const TypeSymbol* baseTs,
                                     llvm::Value* objPtr,
                                     llvm::Value* serPtr) {
    if (!baseTs || !baseTs->decl) return;
    const auto* baseTd = dynamic_cast<const ast::TypeDecl*>(baseTs->decl);
    if (!baseTd || !baseTd->serializedClass) return;
    const auto& polyCases = baseTd->serializedClass->polyCases;
    if (polyCases.empty()) {
        // No poly cases: just serialize the base type directly (degenerate case).
        emitClassSerialize(baseTs, objPtr, serPtr);
        return;
    }

    // Load vptr = *objPtr (vtable pointer at offset 0 of class layout).
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), objPtr, "poly.ser.vptr");
    // Load td = vptr[0] (type descriptor at vtable slot 0).
    llvm::Value* td = builder_.CreateLoad(
        ptrTy(),
        builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(0)}),
        "poly.ser.td");

    // Create end block and (fallthrough) throw block.
    auto* throwBB = llvm::BasicBlock::Create(llvm_, "poly.ser.throw", currentFn_);
    auto* endBB   = llvm::BasicBlock::Create(llvm_, "poly.ser.end",   currentFn_);

    for (const auto& pc : polyCases) {
        if (!pc.type) continue;
        const TypeRef* pcTr = ctx_.resolvedType(*pc.type);
        if (!pcTr || !pcTr->declaration) continue;
        const TypeSymbol* pcTs = pcTr->declaration;
        auto* pcTd = dynamic_cast<const ast::TypeDecl*>(pcTs->decl);
        if (!pcTd || !pcTd->serializedClass) continue;

        // Find the concrete type's descriptor in descArray_.
        auto descIt = descArray_.find(pcTs);
        if (descIt == descArray_.end()) continue;
        llvm::Value* concDesc = llvm::cast<llvm::Value>(descIt->second);

        // Compare td with the concrete type's descriptor.
        llvm::Value* isMatch = builder_.CreateICmpEQ(td, concDesc, "poly.ser.match");
        auto* matchBB = llvm::BasicBlock::Create(llvm_, "poly.ser.case", currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "poly.ser.next", currentFn_);
        builder_.CreateCondBr(isMatch, matchBB, nextBB);

        // Matching branch: serialize the concrete type with $type discriminator.
        builder_.SetInsertPoint(matchBB);
        emitClassSerialize(pcTs, objPtr, serPtr, pc.discriminator);
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(nextBB);
    }

    // No match: raise SerializationException (unregistered dynamic type, §9.8).
    // Per §9.8: "A run-time type not present in the registry is a
    // SerializationException at serialization."
    // Use emitThrowBuiltin which stores the exception and unwinds (unlike
    // bz_ser_throw which only sets bz_exc and returns, requiring a separate
    // emitPendingCheck to propagate it).
    builder_.CreateBr(throwBB);
    builder_.SetInsertPoint(throwBB);
    emitThrowBuiltin("SerializationException");
    // emitThrowBuiltin branches to unwindBB(); add unreachable fallback.
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateUnreachable();

    builder_.SetInsertPoint(endBB);
}

// ---------------------------------------------------------------------------
// Emitter::emitPolyClassDeserialize  §9.8 tier 4
//
// Polymorphic Deserialize dispatch for a base class with a `polymorphic(...)`
// registry.  Reads the "$type" discriminator via bz_binder_read_poly_discriminator,
// compares against registered discriminators, and dispatches to the matching
// concrete type's emitClassDeserialize.
// An unknown discriminator raises SerializationException (§9.8: "closed set").
// Returns an owned ptr typed as the base class (ptr to concrete instance).
// ---------------------------------------------------------------------------
llvm::Value* Emitter::emitPolyClassDeserialize(const TypeSymbol* baseTs,
                                               llvm::Value* derPtr) {
    if (!baseTs || !baseTs->decl)
        return llvm::ConstantPointerNull::get(ptrTy());
    const auto* baseTd = dynamic_cast<const ast::TypeDecl*>(baseTs->decl);
    if (!baseTd || !baseTd->serializedClass)
        return llvm::ConstantPointerNull::get(ptrTy());
    const auto& polyCases = baseTd->serializedClass->polyCases;
    if (polyCases.empty())
        return emitClassDeserialize(baseTs, derPtr);

    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);

    // Read the "$type" discriminator; sets up poly_preread_count for the concrete
    // type's begin_object.  Returns bz_string* discriminator (rc=1).
    auto* readDiscFn = declareC("bz_binder_read_poly_discriminator", ptrTy(),
                                 {ptrTy()});
    llvm::Value* discPtr = builder_.CreateCall(readDiscFn, {derPtr}, "poly.disc");
    emitPendingCheck();

    // Null discriminator → truncated input (SerializationException from bz_binder_read_poly_discriminator).
    {
        llvm::Value* isNull = builder_.CreateICmpEQ(
            discPtr, llvm::ConstantPointerNull::get(ptrTy()), "poly.disc.null");
        auto* nullBB = llvm::BasicBlock::Create(llvm_, "poly.disc.null", currentFn_);
        auto* liveDiscBB = llvm::BasicBlock::Create(llvm_, "poly.disc.live", currentFn_);
        builder_.CreateCondBr(isNull, nullBB, liveDiscBB);
        builder_.SetInsertPoint(nullBB);
        emitThrowBuiltin("SerializationException");
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateUnreachable();
        builder_.SetInsertPoint(liveDiscBB);
    }

    // Register discriminator string for cleanup if something throws.
    // We release it at the end of this function.
    llvm::AllocaInst* discSlot = entryAlloca(ptrTy());
    {
        llvm::BasicBlock& eb = currentFn_->getEntryBlock();
        llvm::IRBuilder<> eb_b(&eb, std::next(discSlot->getIterator()));
        eb_b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), discSlot);
    }
    builder_.CreateStore(discPtr, discSlot);
    if (!disposeStack_.empty()) {
        Disposal nd{discSlot, nullptr};
        nd.stringRelease = true;
        disposeStack_.back().push_back(nd);
    }

    // Allocate a slot for the result ptr.
    llvm::AllocaInst* resultSlot = entryAlloca(ptrTy());
    {
        llvm::BasicBlock& eb = currentFn_->getEntryBlock();
        llvm::IRBuilder<> eb_b(&eb, std::next(resultSlot->getIterator()));
        eb_b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), resultSlot);
    }

    auto* endBB   = llvm::BasicBlock::Create(llvm_, "poly.deser.end",   currentFn_);
    auto* throwBB = llvm::BasicBlock::Create(llvm_, "poly.deser.throw", currentFn_);

    // Build comparison function for disc strings.
    auto* cmpFn = declareC("bz_string_compare", i32(),
                           {ptrTy(), i64(), ptrTy(), i64(), i32()});
    auto* strTy = stringType();

    llvm::Value* discData = builder_.CreateLoad(ptrTy(),
        builder_.CreateStructGEP(strTy, discPtr, 0), "poly.disc.data");
    llvm::Value* discLen  = builder_.CreateLoad(i64(),
        builder_.CreateStructGEP(strTy, discPtr, 1), "poly.disc.len");

    for (const auto& pc : polyCases) {
        if (!pc.type) continue;
        const TypeRef* pcTr = ctx_.resolvedType(*pc.type);
        if (!pcTr || !pcTr->declaration) continue;
        const TypeSymbol* pcTs = pcTr->declaration;
        auto* pcTd = dynamic_cast<const ast::TypeDecl*>(pcTs->decl);
        if (!pcTd || !pcTd->serializedClass) continue;

        // Compare discriminator string with this case's literal.
        llvm::Value* litStr = lowerStringLiteral(
            "\"" + std::string(pc.discriminator) + "\"", false);
        llvm::Value* litData = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(strTy, litStr, 0), "poly.lit.data");
        llvm::Value* litLen  = builder_.CreateLoad(i64(),
            builder_.CreateStructGEP(strTy, litStr, 1), "poly.lit.len");
        llvm::Value* cmpRes = builder_.CreateCall(cmpFn,
            {discData, discLen, litData, litLen, builder_.getInt32(0)}, "poly.cmp");
        llvm::Value* isMatch = builder_.CreateICmpEQ(cmpRes, builder_.getInt32(0), "poly.match");

        auto* matchBB = llvm::BasicBlock::Create(llvm_, "poly.deser.case", currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "poly.deser.next", currentFn_);
        builder_.CreateCondBr(isMatch, matchBB, nextBB);

        builder_.SetInsertPoint(matchBB);
        {
            // Release the discriminator string before dispatching to concrete
            // (concrete's deserialize may throw; the discSlot cleanup handles it).
            auto* relFn = declareC("bz_string_release", voidTy, {ptrTy()});
            builder_.CreateCall(relFn, {discPtr});
            // Null the slot so cleanup doesn't double-release.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), discSlot);

            // Deserialize the concrete type (reads remaining fields + end_object).
            llvm::Value* concretePtr = emitClassDeserialize(pcTs, derPtr);
            builder_.CreateStore(concretePtr, resultSlot);
        }
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(nextBB);
    }

    // No match: unknown discriminator → SerializationException (§9.8).
    builder_.CreateBr(throwBB);
    builder_.SetInsertPoint(throwBB);
    {
        // Release discriminator before throwing.
        auto* relFn = declareC("bz_string_release", voidTy, {ptrTy()});
        builder_.CreateCall(relFn, {discPtr});
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), discSlot);
        emitThrowBuiltin("SerializationException");
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateUnreachable();
    }

    builder_.SetInsertPoint(endBB);
    return builder_.CreateLoad(ptrTy(), resultSlot, "poly.result");
}

// ---------------------------------------------------------------------------
// Emitter::emitClassSerialize  §9.8 tier 2 + tier 3 + tier 4
//
// Synthesized Serialize body for a `serialized tree`/`graph`/`cyclic` class.
// `objPtr` is an opaque ptr to the class instance (ptr to classStruct).
// Iterates the class's serialized components in declaration order, GEPs each
// field, and calls emitSerializeField for its value.
//
// §9.8 tier 3 (graph/cyclic): for each owned class-typed component, writes a
// BZ_SER_INLINE (0x01) + LE32 tag_id marker before the inline body using the
// identity-tag table in the serializer.  For back-edge (non-owned class-typed)
// components, writes BZ_SER_REF (0x02) + LE32 tag_id referencing the existing
// tag of the borrowed object.
//
// §9.8 tier 4 (polymorphic): when `polyDiscriminator` is non-empty, the "$type"
// field (with `polyDiscriminator` as value) is written as the first field in the
// serialized object.  The BeginObject field count is incremented by 1 accordingly.
// This is called per dispatch branch from emitPolyClassSerialize.
// ---------------------------------------------------------------------------
void Emitter::emitClassSerialize(const TypeSymbol* ts,
                                 llvm::Value* objPtr,
                                 llvm::Value* serPtr,
                                 std::string_view polyDiscriminator) {
    if (!ts || !ts->decl) return;

    // §9.8 v0.141 / #703: guard unbounded inline-emission recursion. A class
    // reached again through one of its own components (a `Shared<T>` field of its
    // own type — a linked list or cycle spine, or a recursive owned component)
    // would re-enter here forever. Report a clean "not yet implemented" error
    // (supporting it requires a synthesized Serialize FUNCTION, not inlining).
    if (serializeEmitStack_.count(ts)) {
        ctx_.diagnostics().report(
            Severity::Error, ts->declLoc,
            "class `" + std::string(ts->name) + "`: recursive serialization "
            "(its type is reached again through a `Shared<T>`/`Weak<T>` or owned "
            "component \xE2\x80\x94 a linked list or cycle spine) is not yet "
            "implemented in codegen (\xC2\xA7""9.8 v0.141, issue #703 \xE2\x80\x94 "
            "inline emission would recurse; a synthesized Serialize function is "
            "required)");
        return;
    }
    serializeEmitStack_.insert(ts);
    struct PopSer {
        std::unordered_set<const sema::TypeSymbol*>& s;
        const sema::TypeSymbol* t;
        ~PopSer() { s.erase(t); }
    } popSer{serializeEmitStack_, ts};

    // Detect graph mode: true for `serialized graph` and `serialized cyclic`.
    const auto* td0 = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    bool isGraphMode = td0 && td0->serializedClass &&
                       (td0->serializedClass->shape == "graph" ||
                        td0->serializedClass->shape == "cyclic");

    // Ensure classStruct is built and fieldIndex_ is populated.
    llvm::StructType* st = classStruct(ts);

    std::vector<const Symbol*> storage;
    collectInstanceStorage(ts, storage);

    std::vector<ClassComponent> comps;
    collectClassComponents(ts, storage, fieldIndex_, ctx_, comps, isGraphMode);

    // §9.8 v0.141 / #703: Shared<T>/Weak<T> components are eligibility-accepted
    // (sema slice S1). Slice S2 emits Shared<C> identity codegen (handled below);
    // the remaining cases (Weak<T>, nullable Shared<C>?, Shared<non-class>) are
    // not yet emitted, so guard them with a clean error rather than miscompiling
    // a builtin handle as an owning class component.
    for (const auto& comp : comps) {
        if (comp.isSkip) continue;
        if (isSharedOrWeakComponentType(comp.ty) &&
            !isHandledTypeDrivenComponent(comp, isGraphMode)) {
            ctx_.diagnostics().report(
                Severity::Error, ts->declLoc,
                "class `" + std::string(ts->name) + "`: serialization of this "
                "`Shared<T>`/`Weak<T>` component (field `" + comp.declName +
                "`) is not yet implemented in codegen (\xC2\xA7""9.8 v0.141, "
                "issue #703 \xE2\x80\x94 only non-nullable `Shared<class>` in "
                "graph mode is supported so far; Weak/nullable/value-element land "
                "in a later slice)");
            return;
        }
    }

    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);

    // Determine field count for BeginObject.  Non-skip, non-optional fields are
    // always included.  Optional nullable fields are included only when non-null
    // (§9.8: "serialized optional: omit null on write").  If there are optional
    // fields, the count must be computed at runtime.
    uint32_t staticCount = 0;      // non-skip non-optional fields (always written)
    bool hasOptionals = false;
    for (const auto& comp : comps) {
        if (comp.isSkip) continue;
        if (!comp.ty) continue;
        if (comp.isOptional && comp.ty->nullable) { hasOptionals = true; continue; }
        ++staticCount;
    }

    // Pre-load all optional nullable fields' null flags to compute runtime count.
    // For each optional comp, we load the field value and check for null.
    // Build: count = staticCount + sum(optional_i is non-null ? 1 : 0)
    struct OptInfo {
        llvm::Value* isNonNull;  // i1: true = non-null (should be written)
        std::size_t compIdx;     // index into comps
    };
    std::vector<OptInfo> optInfos;
    if (hasOptionals) {
        for (std::size_t i = 0; i < comps.size(); ++i) {
            const auto& comp = comps[i];
            if (comp.isSkip || !comp.ty) continue;
            if (!(comp.isOptional && comp.ty->nullable)) continue;
            llvm::Type* llvmFty = lowerTypeRef(comp.ty);
            if (!llvmFty) continue;
            llvm::Value* fieldPtr =
                builder_.CreateStructGEP(st, objPtr, comp.fieldIdx, "cser.ofp");
            llvm::Value* fieldVal = builder_.CreateLoad(llvmFty, fieldPtr, "cser.ofv");
            llvm::Value* isNull;
            if (fieldVal->getType()->isPointerTy()) {
                isNull = builder_.CreateICmpEQ(
                    fieldVal, llvm::ConstantPointerNull::get(ptrTy()), "cser.oisnull");
            } else if (fieldVal->getType()->isStructTy()) {
                llvm::Value* hv = builder_.CreateExtractValue(fieldVal, {0u}, "cser.ohv");
                isNull = builder_.CreateICmpEQ(hv, builder_.getInt8(0), "cser.oisnull");
            } else {
                isNull = builder_.getFalse();
            }
            llvm::Value* isNonNull = builder_.CreateNot(isNull, "cser.onnull");
            optInfos.push_back({isNonNull, i});
        }
    }

    // Compute the dynamic field count.
    // §9.8 tier 4: when polyDiscriminator is non-empty, add 1 for the leading
    // "$type" field written before all component fields.
    int32_t polyOffset = polyDiscriminator.empty() ? 0 : 1;
    llvm::Value* dynCount = builder_.getInt32((int32_t)staticCount + polyOffset);
    for (const auto& oi : optInfos) {
        llvm::Value* inc = builder_.CreateZExt(oi.isNonNull, i32(), "cser.inc");
        dynCount = builder_.CreateAdd(dynCount, inc, "cser.cnt");
    }

    // BeginObject(dynamic_field_count).
    auto* beginFn = declareC("bz_binser_begin_object", voidTy, {ptrTy(), i32()});
    builder_.CreateCall(beginFn, {serPtr, dynCount});
    // §2.15 / fix #687: bz_binser_begin_object can now set bz_exc (MaxDepth guard).
    // Check for pending exception so a try/catch SerializationException can catch it.
    emitPendingCheck();

    // §9.8 tier 4: write "$type" discriminator field as the leading field.
    // Per §9.8: "the discriminator field name is '$type', reserved within an object
    // written for a polymorphic base; it precedes all component fields."
    if (!polyDiscriminator.empty()) {
        auto* writeNameFn = declareC("bz_binser_write_field_name", voidTy,
                                     {ptrTy(), ptrTy()});
        auto* writeStrFn  = declareC("bz_binser_write_string", voidTy,
                                     {ptrTy(), ptrTy()});
        llvm::Value* typeFieldName = lowerStringLiteral("\"$type\"", false);
        builder_.CreateCall(writeNameFn, {serPtr, typeFieldName});
        llvm::Value* discStr = lowerStringLiteral(
            "\"" + std::string(polyDiscriminator) + "\"", false);
        builder_.CreateCall(writeStrFn, {serPtr, discStr});
    }

    // Map optional comp index to its isNonNull flag for the write-gate check.
    std::unordered_map<std::size_t, llvm::Value*> optNonNullMap;
    for (const auto& oi : optInfos)
        optNonNullMap[oi.compIdx] = oi.isNonNull;

    for (std::size_t i = 0; i < comps.size(); ++i) {
        const auto& comp = comps[i];
        if (comp.isSkip) continue;
        if (!comp.ty) continue;

        llvm::Value* fieldPtr =
            builder_.CreateStructGEP(st, objPtr, comp.fieldIdx, "cser.fp");
        const sema::TypeRef* fty = comp.ty;
        llvm::Type* llvmFty = lowerTypeRef(fty);
        if (!llvmFty) continue;

        // §9.8 tier 3: back-edge fields (non-owned class-typed) in graph/cyclic
        // mode.  Write field-name + BZ_SER_REF marker + existing tag_id.
        // The target object was already written as an owned component earlier
        // (owner-before-borrower order guaranteed by declaration order per §9.8).
        if (isGraphMode && comp.isBackEdge) {
            // Load the back-edge ptr from the field.
            llvm::Value* bePtr = builder_.CreateLoad(ptrTy(), fieldPtr, "cser.be");
            // Write field name.
            llvm::Value* nameStr = lowerStringLiteral(
                "\"" + comp.wireName + "\"", false);
            auto* writeNameFn = declareC("bz_binser_write_field_name", voidTy,
                                         {ptrTy(), ptrTy()});
            builder_.CreateCall(writeNameFn, {serPtr, nameStr});
            // §9.8 tier 3: nullable back-edge — if ptr is null, write null marker
            // (0x00 via bz_binser_write_null); otherwise write BZ_SER_REF + tag_id.
            llvm::Value* isNullBe = builder_.CreateICmpEQ(
                bePtr, llvm::ConstantPointerNull::get(ptrTy()), "cser.be.isnull");
            auto* beNullBB = llvm::BasicBlock::Create(llvm_, "cser.be.null", currentFn_);
            auto* beRefBB  = llvm::BasicBlock::Create(llvm_, "cser.be.ref",  currentFn_);
            auto* beEndBB  = llvm::BasicBlock::Create(llvm_, "cser.be.end",  currentFn_);
            builder_.CreateCondBr(isNullBe, beNullBB, beRefBB);
            // Null path: write 0x00 null marker (no tag_id follows).
            builder_.SetInsertPoint(beNullBB);
            {
                auto* writeNullFn = declareC("bz_binser_write_null", voidTy, {ptrTy()});
                builder_.CreateCall(writeNullFn, {serPtr});
            }
            builder_.CreateBr(beEndBB);
            // Non-null path: look up existing tag, write BZ_SER_REF + tag_id.
            // §9.8 cycle detection: tag_id=0 means the back-edge target was never
            // registered as an owned component — it forms a cycle.  Raise
            // SerializationException instead of emitting a dangling $ref.
            builder_.SetInsertPoint(beRefBB);
            {
                auto* getTagFn = declareC("bz_binser_graph_get_tag", i32(),
                                          {ptrTy(), ptrTy()});
                llvm::Value* existingTag = builder_.CreateCall(
                    getTagFn, {serPtr, bePtr}, "cser.be.tag");
                llvm::Value* isCycle = builder_.CreateICmpEQ(
                    existingTag, builder_.getInt32(0), "cser.be.cycle");
                auto* cycleBB   = llvm::BasicBlock::Create(llvm_, "cser.be.cyclic", currentFn_);
                auto* noRefBB   = llvm::BasicBlock::Create(llvm_, "cser.be.noref",  currentFn_);
                builder_.CreateCondBr(isCycle, cycleBB, noRefBB);
                // Cycle: raise SerializationException.
                builder_.SetInsertPoint(cycleBB);
                emitThrowBuiltin("SerializationException");
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateUnreachable();
                // No cycle: write the $ref.
                builder_.SetInsertPoint(noRefBB);
                auto* writeRefFn = declareC("bz_binser_write_ref_tag", voidTy,
                                            {ptrTy(), i32()});
                builder_.CreateCall(writeRefFn, {serPtr, existingTag});
            }
            builder_.CreateBr(beEndBB);
            builder_.SetInsertPoint(beEndBB);
            continue;
        }

        // §9.8 v0.141 / #703 slice S2: Shared<C> identity participant.
        // Key the tag on the underlying VALUE (bz_shared_get) so aliased clones
        // unify with each other (and with any owned reference to the same value).
        // First encounter: assign tag, write BZ_SER_INLINE + tag_id, then write
        // the element's inline object body. Alias: write BZ_SER_REF + tag_id.
        if (isGraphMode) {
            if (const TypeSymbol* elemTs = handledSharedElementClass(fty)) {
                llvm::Value* handle =
                    builder_.CreateLoad(ptrTy(), fieldPtr, "cser.sh.h");
                // Field name.
                llvm::Value* nameStr = lowerStringLiteral(
                    "\"" + comp.wireName + "\"", false);
                auto* writeNameFn = declareC("bz_binser_write_field_name", voidTy,
                                             {ptrTy(), ptrTy()});
                builder_.CreateCall(writeNameFn, {serPtr, nameStr});
                auto* shDoneBB =
                    llvm::BasicBlock::Create(llvm_, "cser.sh.done", currentFn_);
                // §9.8 v0.141 / #703 slice S4: a nullable Shared<C>? whose handle
                // is null writes the null marker; otherwise fall through to the
                // identity logic below.
                if (fty->nullable) {
                    auto* writeNullFn =
                        declareC("bz_binser_write_null", voidTy, {ptrTy()});
                    llvm::Value* hNull = builder_.CreateICmpEQ(
                        handle, llvm::ConstantPointerNull::get(ptrTy()),
                        "cser.sh.hnull");
                    auto* shNullBB = llvm::BasicBlock::Create(
                        llvm_, "cser.sh.hnullw", currentFn_);
                    auto* shLiveBB = llvm::BasicBlock::Create(
                        llvm_, "cser.sh.live", currentFn_);
                    builder_.CreateCondBr(hNull, shNullBB, shLiveBB);
                    builder_.SetInsertPoint(shNullBB);
                    builder_.CreateCall(writeNullFn, {serPtr});
                    builder_.CreateBr(shDoneBB);
                    builder_.SetInsertPoint(shLiveBB);
                }
                // Identity key = bz_shared_get(handle) (the inner value ptr).
                auto* getFn = declareC("bz_shared_get", ptrTy(), {ptrTy()});
                llvm::Value* val =
                    builder_.CreateCall(getFn, {handle}, "cser.sh.val");
                auto* getTagFn = declareC("bz_binser_graph_get_tag", i32(),
                                          {ptrTy(), ptrTy()});
                llvm::Value* existingTag = builder_.CreateCall(
                    getTagFn, {serPtr, val}, "cser.sh.tag");
                llvm::Value* isNew = builder_.CreateICmpEQ(
                    existingTag, builder_.getInt32(0), "cser.sh.isnew");
                auto* shInlineBB =
                    llvm::BasicBlock::Create(llvm_, "cser.sh.inline", currentFn_);
                auto* shRefBB =
                    llvm::BasicBlock::Create(llvm_, "cser.sh.ref", currentFn_);
                builder_.CreateCondBr(isNew, shInlineBB, shRefBB);
                // First encounter: assign tag, write inline marker + body.
                builder_.SetInsertPoint(shInlineBB);
                {
                    auto* assignFn = declareC("bz_binser_graph_assign_tag", i32(),
                                              {ptrTy(), ptrTy()});
                    llvm::Value* newTag = builder_.CreateCall(
                        assignFn, {serPtr, val}, "cser.sh.newtag");
                    auto* writeInlineFn = declareC("bz_binser_write_inline_tag",
                                                   voidTy, {ptrTy(), i32()});
                    builder_.CreateCall(writeInlineFn, {serPtr, newTag});
                    emitClassSerialize(elemTs, val, serPtr);
                }
                builder_.CreateBr(shDoneBB);
                // Alias: write $ref to the existing tag.
                builder_.SetInsertPoint(shRefBB);
                {
                    auto* writeRefFn = declareC("bz_binser_write_ref_tag", voidTy,
                                                {ptrTy(), i32()});
                    builder_.CreateCall(writeRefFn, {serPtr, existingTag});
                }
                builder_.CreateBr(shDoneBB);
                builder_.SetInsertPoint(shDoneBB);
                continue;
            }
            // §9.8 v0.141 / #703 slice S3: owned Weak<C> back-reference. Write a
            // $ref to the tag of the Shared<C> participant that owns the same
            // value (keyed on bz_shared_get of the weak handle). A null weak —
            // or one whose target was not serialized as a participant (untagged)
            // — writes the null marker (read back as a null weak; the strict P5
            // ordering backstop, throw-on-untagged, is a refinement deferred to a
            // later slice).
            if (comp.isOwned && handledWeakElementClass(fty)) {
                llvm::Value* wh =
                    builder_.CreateLoad(ptrTy(), fieldPtr, "cser.wk.h");
                llvm::Value* nameStr = lowerStringLiteral(
                    "\"" + comp.wireName + "\"", false);
                auto* writeNameFn = declareC("bz_binser_write_field_name", voidTy,
                                             {ptrTy(), ptrTy()});
                builder_.CreateCall(writeNameFn, {serPtr, nameStr});
                auto* writeNullFn =
                    declareC("bz_binser_write_null", voidTy, {ptrTy()});
                llvm::Value* whNull = builder_.CreateICmpEQ(
                    wh, llvm::ConstantPointerNull::get(ptrTy()), "cser.wk.isnull");
                auto* wkNullBB =
                    llvm::BasicBlock::Create(llvm_, "cser.wk.null", currentFn_);
                auto* wkLiveBB =
                    llvm::BasicBlock::Create(llvm_, "cser.wk.live", currentFn_);
                auto* wkDoneBB =
                    llvm::BasicBlock::Create(llvm_, "cser.wk.done", currentFn_);
                builder_.CreateCondBr(whNull, wkNullBB, wkLiveBB);
                // Null weak.
                builder_.SetInsertPoint(wkNullBB);
                builder_.CreateCall(writeNullFn, {serPtr});
                builder_.CreateBr(wkDoneBB);
                // Live weak: look up the participant tag of its value.
                builder_.SetInsertPoint(wkLiveBB);
                {
                    auto* getFn = declareC("bz_shared_get", ptrTy(), {ptrTy()});
                    llvm::Value* wv =
                        builder_.CreateCall(getFn, {wh}, "cser.wk.val");
                    auto* getTagFn = declareC("bz_binser_graph_get_tag", i32(),
                                              {ptrTy(), ptrTy()});
                    llvm::Value* wtag = builder_.CreateCall(
                        getTagFn, {serPtr, wv}, "cser.wk.tag");
                    llvm::Value* untagged = builder_.CreateICmpEQ(
                        wtag, builder_.getInt32(0), "cser.wk.untagged");
                    auto* wkUntagBB = llvm::BasicBlock::Create(
                        llvm_, "cser.wk.untag", currentFn_);
                    auto* wkRefBB = llvm::BasicBlock::Create(
                        llvm_, "cser.wk.ref", currentFn_);
                    builder_.CreateCondBr(untagged, wkUntagBB, wkRefBB);
                    builder_.SetInsertPoint(wkUntagBB);
                    builder_.CreateCall(writeNullFn, {serPtr});
                    builder_.CreateBr(wkDoneBB);
                    builder_.SetInsertPoint(wkRefBB);
                    auto* writeRefFn = declareC("bz_binser_write_ref_tag", voidTy,
                                                {ptrTy(), i32()});
                    builder_.CreateCall(writeRefFn, {serPtr, wtag});
                    builder_.CreateBr(wkDoneBB);
                }
                builder_.SetInsertPoint(wkDoneBB);
                continue;
            }
        }

        // Handle `serialized optional`: on write, omit null nullable fields.
        if (comp.isOptional && fty->nullable) {
            // The isNonNull flag was already computed; use it as the write-gate.
            auto nit = optNonNullMap.find(i);
            llvm::Value* isNonNull = (nit != optNonNullMap.end())
                ? nit->second : builder_.getTrue();
            auto* writeBB = llvm::BasicBlock::Create(llvm_, "cser.opt.write", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "cser.opt.end",   currentFn_);
            builder_.CreateCondBr(isNonNull, writeBB, endBB);
            builder_.SetInsertPoint(writeBB);
            {
                // Re-load the field value (using the same fieldPtr already computed).
                llvm::Value* fieldVal = builder_.CreateLoad(llvmFty, fieldPtr, "cser.fv");
                llvm::Value* nameStr = lowerStringLiteral(
                    "\"" + comp.wireName + "\"", false);
                auto* writeNameFn = declareC("bz_binser_write_field_name", voidTy,
                                             {ptrTy(), ptrTy()});
                builder_.CreateCall(writeNameFn, {serPtr, nameStr});
                emitSerializeField(fieldVal, fty, serPtr);
            }
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            continue;
        }

        // Standard (non-optional) field: always write.
        llvm::Value* fieldVal = builder_.CreateLoad(llvmFty, fieldPtr, "cser.fv");
        llvm::Value* nameStr = lowerStringLiteral(
            "\"" + comp.wireName + "\"", false);
        auto* writeNameFn = declareC("bz_binser_write_field_name", voidTy,
                                     {ptrTy(), ptrTy()});
        builder_.CreateCall(writeNameFn, {serPtr, nameStr});

        // §9.8 tier 3: in graph/cyclic mode, owned class-typed components get
        // an identity-tag marker before their inline body.  This allows the
        // deserializer to build a tag→object table for back-edge resolution.
        if (isGraphMode && comp.isOwned &&
            fty->kind == sema::TypeRef::Kind::Named &&
            fty->declaration &&
            fty->declaration->kind == ast::TypeDeclKind::Class) {
            // fieldVal is the owned ptr.  Check if already tagged (shouldn't be
            // for tree-structured owned edges, but handle it gracefully).
            auto* getTagFn = declareC("bz_binser_graph_get_tag", i32(),
                                      {ptrTy(), ptrTy()});
            llvm::Value* existingTag = builder_.CreateCall(
                getTagFn, {serPtr, fieldVal}, "cser.ot.chk");
            llvm::Value* isNew = builder_.CreateICmpEQ(
                existingTag, builder_.getInt32(0), "cser.ot.isnew");
            auto* inlineBB = llvm::BasicBlock::Create(llvm_, "cser.ot.inline", currentFn_);
            auto* refBB    = llvm::BasicBlock::Create(llvm_, "cser.ot.ref",    currentFn_);
            auto* doneTagBB = llvm::BasicBlock::Create(llvm_, "cser.ot.done",  currentFn_);
            builder_.CreateCondBr(isNew, inlineBB, refBB);

            // First reach: assign tag, write inline marker, then write body.
            builder_.SetInsertPoint(inlineBB);
            {
                auto* assignFn = declareC("bz_binser_graph_assign_tag", i32(),
                                          {ptrTy(), ptrTy()});
                llvm::Value* newTag = builder_.CreateCall(
                    assignFn, {serPtr, fieldVal}, "cser.ot.tag");
                auto* writeInlineFn = declareC("bz_binser_write_inline_tag", voidTy,
                                               {ptrTy(), i32()});
                builder_.CreateCall(writeInlineFn, {serPtr, newTag});
                emitSerializeField(fieldVal, fty, serPtr);
            }
            builder_.CreateBr(doneTagBB);

            // Already seen (aliased in tree path — should not happen for well-formed
            // serialized graph, but handle defensively): write $ref.
            builder_.SetInsertPoint(refBB);
            {
                auto* writeRefFn = declareC("bz_binser_write_ref_tag", voidTy,
                                            {ptrTy(), i32()});
                builder_.CreateCall(writeRefFn, {serPtr, existingTag});
            }
            builder_.CreateBr(doneTagBB);

            builder_.SetInsertPoint(doneTagBB);
            continue;
        }

        emitSerializeField(fieldVal, fty, serPtr);
    }

    // EndObject.
    auto* endFn = declareC("bz_binser_end_object", voidTy, {ptrTy()});
    builder_.CreateCall(endFn, {serPtr});
}

// ---------------------------------------------------------------------------
// Emitter::emitClassDeserialize  §9.8 tier 2 + tier 3
//
// Synthesized Deserialize body for a `serialized tree`/`graph`/`cyclic` class.
// Returns an owned ptr to the newly constructed class instance.
//
// Algorithm (tree mode):
//   1. Read all non-skip serialized components into local slots.
//   2. Allocate the class via calloc + emitInstanceInit.
//   3. Find the unique constructor matching serialized components by name.
//   4. Call that constructor with the deserialized values.
//   5. Return the owned ptr.
//
// §9.8 tier 3 additional (graph/cyclic mode):
//   - For owned class-typed components: read BZ_SER_INLINE marker + tag_id,
//     mark tag as in-progress, deserialize body, register tag.
//   - For back-edge components: read BZ_SER_REF marker + tag_id,
//     resolve tag from table (NULL = cycle → SerializationException),
//     pass ptr to constructor.
// ---------------------------------------------------------------------------
llvm::Value* Emitter::emitClassDeserialize(const TypeSymbol* ts,
                                           llvm::Value* derPtr) {
    if (!ts || !ts->decl) return llvm::ConstantPointerNull::get(ptrTy());

    // §9.8 v0.141 / #703: guard unbounded inline-emission recursion (see the
    // matching guard in emitClassSerialize).
    if (deserializeEmitStack_.count(ts)) {
        ctx_.diagnostics().report(
            Severity::Error, ts->declLoc,
            "class `" + std::string(ts->name) + "`: recursive deserialization "
            "(its type is reached again through a `Shared<T>`/`Weak<T>` or owned "
            "component \xE2\x80\x94 a linked list or cycle spine) is not yet "
            "implemented in codegen (\xC2\xA7""9.8 v0.141, issue #703 \xE2\x80\x94 "
            "inline emission would recurse; a synthesized Deserialize function is "
            "required)");
        return llvm::ConstantPointerNull::get(ptrTy());
    }
    deserializeEmitStack_.insert(ts);
    struct PopDes {
        std::unordered_set<const sema::TypeSymbol*>& s;
        const sema::TypeSymbol* t;
        ~PopDes() { s.erase(t); }
    } popDes{deserializeEmitStack_, ts};

    // Detect graph mode.
    const auto* td0g = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    bool isGraphMode = td0g && td0g->serializedClass &&
                       (td0g->serializedClass->shape == "graph" ||
                        td0g->serializedClass->shape == "cyclic");

    llvm::StructType* st = classStruct(ts);
    llvm::Type* voidTy   = llvm::Type::getVoidTy(llvm_);

    std::vector<const Symbol*> storage;
    collectInstanceStorage(ts, storage);

    std::vector<ClassComponent> comps;
    collectClassComponents(ts, storage, fieldIndex_, ctx_, comps, isGraphMode);

    // §9.8 v0.141 / #703: Shared<T>/Weak<T> components are eligibility-accepted
    // (sema slice S1). Slice S2 emits Shared<C> identity codegen (handled below);
    // guard the remaining cases with a clean error rather than miscompiling.
    for (const auto& comp : comps) {
        if (comp.isSkip) continue;
        if (isSharedOrWeakComponentType(comp.ty) &&
            !isHandledTypeDrivenComponent(comp, isGraphMode)) {
            ctx_.diagnostics().report(
                Severity::Error, ts->declLoc,
                "class `" + std::string(ts->name) + "`: deserialization of this "
                "`Shared<T>`/`Weak<T>` component (field `" + comp.declName +
                "`) is not yet implemented in codegen (\xC2\xA7""9.8 v0.141, "
                "issue #703 \xE2\x80\x94 only non-nullable `Shared<class>` in "
                "graph mode is supported so far; Weak/nullable/value-element land "
                "in a later slice)");
            return llvm::ConstantPointerNull::get(ptrTy());
        }
    }

    // Allocate local slots for each non-skip component.
    // fix #669: null-init stores must be in the entry block, not the current
    // insert point (which may be liveBB of an outer nullable deserialization).
    std::vector<llvm::AllocaInst*> slots(comps.size(), nullptr);
    for (unsigned i = 0; i < comps.size(); ++i) {
        if (comps[i].isSkip || !comps[i].ty) continue;
        llvm::Type* lt = lowerTypeRef(comps[i].ty);
        if (!lt) continue;
        llvm::AllocaInst* slot = entryAlloca(lt);
        {
            llvm::BasicBlock& eb = currentFn_->getEntryBlock();
            llvm::IRBuilder<> eb_b(&eb, std::next(slot->getIterator()));
            eb_b.CreateStore(llvm::Constant::getNullValue(lt), slot);
        }
        slots[i] = slot;
    }

    // fix #668: §9.8 / §2.15: "missing required fields remain unconditional
    // errors."  For each non-skip, non-optional component allocate an i8 seen-
    // flag alloca (entry-block init to 0); the flag is set to 1 when the field
    // is matched in the loop.  After EndObject, any flag still 0 means a
    // required component was absent → raise SerializationException.
    //
    // Nullable T? fields without `serialized optional` are REQUIRED: nullability
    // does not confer optionality.  Only the `serialized optional` clause exempts
    // a field from the missing-field check (§9.8: "serialized optional is the
    // per-field missing-field tolerant-mode opt-in").
    struct SeenEntry {
        std::size_t compIdx;      // index into comps[]
        std::string declName;     // for the error message
        llvm::AllocaInst* slot;   // i8 flag: 0=unseen, 1=seen
    };
    std::vector<SeenEntry> seenFlags;
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].isSkip) continue;      // skip excluded from wire
        if (comps[i].isOptional) continue;  // optional: missing is fine
        // Required component: allocate a seen-flag, zero in entry block.
        llvm::AllocaInst* sf = entryAlloca(i8());
        {
            llvm::BasicBlock& eb = currentFn_->getEntryBlock();
            llvm::IRBuilder<> eb_b(&eb, std::next(sf->getIterator()));
            eb_b.CreateStore(builder_.getInt8(0), sf);
        }
        seenFlags.push_back({i, comps[i].declName, sf});
    }

    // §9.8 / #550 leak-safety: the per-iteration field-name string returned by
    // bz_binder_read_field_name must be released even when a nested
    // emitClassDeserialize throws (its emitThrowBuiltin calls emitUnwindDisposals
    // which sweeps ALL disposeStack_ entries, including this one).
    // Strategy: keep one `namePtrSlot` alloca (null-initialized) that holds the
    // current iteration's namePtr.  Register it as a stringRelease disposal so
    // emitUnwindDisposals automatically calls bz_string_release on it.  On the
    // normal match/skip path, after the explicit bz_string_release(namePtr), we
    // null the slot to prevent the disposal from double-releasing.
    //
    // fix #669: null-init MUST be emitted in the function entry block, not at
    // the current insert point.  emitClassDeserialize can be called from inside
    // a conditional branch (e.g. the live-path of emitDeserializeField for a
    // nullable class T? component).  entryAlloca() places the alloca in the entry
    // block (always runs), but if the null-init store is emitted at the current
    // insert point — which is liveBB — it never runs on the null path.  The
    // alloca then holds uninitialised stack bytes, and the disposeStack_
    // stringRelease mechanism later calls bz_string_release on garbage → SIGSEGV
    // in AOT mode (JIT's zeroed heap happens to mask it).  By emitting the store
    // directly into the entry block, the slot is always null before any live-path
    // code runs.  Per §9.8/§28.
    llvm::AllocaInst* namePtrSlot = entryAlloca(ptrTy());
    {
        // Insert the null-init store into the entry block right after the alloca.
        llvm::BasicBlock& eb = currentFn_->getEntryBlock();
        llvm::IRBuilder<> eb_b(&eb, std::next(namePtrSlot->getIterator()));
        eb_b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), namePtrSlot);
    }
    if (!disposeStack_.empty()) {
        Disposal nd{namePtrSlot, nullptr};
        nd.stringRelease = true;
        disposeStack_.back().push_back(nd);
    }

    // BeginObject → read field count.
    auto* beginFn = declareC("bz_binder_begin_object", i32(), {ptrTy()});
    llvm::Value* fieldCount = builder_.CreateCall(beginFn, {derPtr}, "cdeser.cnt");
    // §2.15: bz_binder_begin_object can throw (JSON: missing '{', truncated).
    emitPendingCheck();

    // Loop over fieldCount, reading each field name and dispatching.
    auto* loopHdr  = llvm::BasicBlock::Create(llvm_, "cdeser.hdr",  currentFn_);
    auto* loopBody = llvm::BasicBlock::Create(llvm_, "cdeser.body", currentFn_);
    auto* loopEnd  = llvm::BasicBlock::Create(llvm_, "cdeser.lend", currentFn_);

    llvm::AllocaInst* idxSlot = entryAlloca(i32());
    builder_.CreateStore(builder_.getInt32(0), idxSlot);
    builder_.CreateBr(loopHdr);

    builder_.SetInsertPoint(loopHdr);
    llvm::Value* idx  = builder_.CreateLoad(i32(), idxSlot, "cdeser.idx");
    llvm::Value* cond = builder_.CreateICmpSLT(idx, fieldCount, "cdeser.more");
    builder_.CreateCondBr(cond, loopBody, loopEnd);

    builder_.SetInsertPoint(loopBody);

    auto* readNameFn = declareC("bz_binder_read_field_name", ptrTy(), {ptrTy()});
    llvm::Value* namePtr = builder_.CreateCall(readNameFn, {derPtr}, "cdeser.fname");

    // §9.8 / #550: truncated input — bz_binder_read_field_name returns null.
    // Dispose any already-allocated owned-class slots, raise SerializationException.
    {
        llvm::Value* isNull = builder_.CreateICmpEQ(
            namePtr, llvm::ConstantPointerNull::get(ptrTy()), "cdeser.nfname");
        auto* errBB  = llvm::BasicBlock::Create(llvm_, "cdeser.trunc", currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "cdeser.cont",  currentFn_);
        builder_.CreateCondBr(isNull, errBB, contBB);

        builder_.SetInsertPoint(errBB);
        // Dispose already-filled slots (prevent leaks on truncated-input path).
        // §28.3/§28.5: owned class ptrs must be disposed; §4.6 string ptrs must
        // be released (bz_string_release); other value types need no cleanup.
        auto* voidTy2 = llvm::Type::getVoidTy(llvm_);
        for (std::size_t ci2 = 0; ci2 < comps.size(); ++ci2) {
            if (comps[ci2].isSkip || !slots[ci2]) continue;
            if (!comps[ci2].ty) continue;
            const auto* ty2 = comps[ci2].ty;
            // Owned class: Dispose + free.
            if (ty2->kind == sema::TypeRef::Kind::Named) {
                const TypeSymbol* cTs = ty2->declaration;
                if (!cTs || cTs->kind != ast::TypeDeclKind::Class) continue;
                llvm::Value* p = builder_.CreateLoad(ptrTy(), slots[ci2], "cdeser.pslot");
                llvm::Value* nonNull = builder_.CreateICmpNE(
                    p, llvm::ConstantPointerNull::get(ptrTy()), "cdeser.pnn");
                auto* dispBB    = llvm::BasicBlock::Create(llvm_, "cdeser.disp",  currentFn_);
                auto* skipBB2   = llvm::BasicBlock::Create(llvm_, "cdeser.skipt", currentFn_);
                builder_.CreateCondBr(nonNull, dispBB, skipBB2);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(p);
                builder_.CreateBr(skipBB2);
                builder_.SetInsertPoint(skipBB2);
                continue;
            }
            // String: bz_string_release (null-safe per runtime implementation).
            if (ty2->kind == sema::TypeRef::Kind::Primitive &&
                ty2->primitive == sema::PrimitiveType::String) {
                llvm::Value* sp = builder_.CreateLoad(ptrTy(), slots[ci2], "cdeser.sslot");
                llvm::Value* nonNull = builder_.CreateICmpNE(
                    sp, llvm::ConstantPointerNull::get(ptrTy()), "cdeser.snn");
                auto* relBB  = llvm::BasicBlock::Create(llvm_, "cdeser.srel",  currentFn_);
                auto* skipBB2 = llvm::BasicBlock::Create(llvm_, "cdeser.sskip", currentFn_);
                builder_.CreateCondBr(nonNull, relBB, skipBB2);
                builder_.SetInsertPoint(relBB);
                auto* relFn2 = declareC("bz_string_release", voidTy2, {ptrTy()});
                builder_.CreateCall(relFn2, {sp});
                builder_.CreateBr(skipBB2);
                builder_.SetInsertPoint(skipBB2);
                continue;
            }
        }
        // Raise SerializationException (sets bz_exc, unwinds, branches to unwindBB).
        emitThrowBuiltin("SerializationException");
        // emitThrowBuiltin branches to unwindBB; add unreachable as fallback.
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateUnreachable();

        builder_.SetInsertPoint(contBB);
        // Record the live namePtr so emitUnwindDisposals can release it if a
        // nested deserialization throws before the normal match/skip branch.
        builder_.CreateStore(namePtr, namePtrSlot);
    }

    auto* cmpFn = declareC("bz_string_compare", i32(),
                           {ptrTy(), i64(), ptrTy(), i64(), i32()});
    auto* strTy = stringType();

    auto* skipBB  = llvm::BasicBlock::Create(llvm_, "cdeser.skip",  currentFn_);
    auto* afterBB = llvm::BasicBlock::Create(llvm_, "cdeser.after", currentFn_);

    for (unsigned ci = 0; ci < comps.size(); ++ci) {
        const ClassComponent& comp = comps[ci];
        if (comp.isSkip || !slots[ci]) continue;

        const std::string& wn = comp.wireName;
        llvm::Value* litStr = lowerStringLiteral("\"" + wn + "\"", false);

        llvm::Value* litData  = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(strTy, litStr, 0), "clit.data");
        llvm::Value* litLen   = builder_.CreateLoad(i64(),
            builder_.CreateStructGEP(strTy, litStr, 1), "clit.len");
        llvm::Value* nameData = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(strTy, namePtr, 0), "cname.data");
        llvm::Value* nameLen  = builder_.CreateLoad(i64(),
            builder_.CreateStructGEP(strTy, namePtr, 1), "cname.len");

        llvm::Value* cmpRes = builder_.CreateCall(cmpFn,
            {nameData, nameLen, litData, litLen, builder_.getInt32(0)}, "cdeser.cmp");
        llvm::Value* isMatch = builder_.CreateICmpEQ(
            cmpRes, builder_.getInt32(0), "cdeser.match");

        auto* readBB = llvm::BasicBlock::Create(llvm_, "cdeser.rd", currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "cdeser.nd", currentFn_);
        builder_.CreateCondBr(isMatch, readBB, nextBB);

        builder_.SetInsertPoint(readBB);
        {
            bool isClassComp = (comp.ty &&
                                comp.ty->kind == sema::TypeRef::Kind::Named &&
                                comp.ty->declaration &&
                                comp.ty->declaration->kind == ast::TypeDeclKind::Class);

            llvm::Value* fv = nullptr;

            // §9.8 v0.141 / #703 slice S2: Shared<C> identity participant.
            // Read the marker: BZ_SER_INLINE → construct the element, wrap it in
            // a NEW Shared control block, and register the HANDLE under the tag
            // (so aliases share one control block). BZ_SER_REF → resolve the tag
            // to the registered handle and bz_shared_clone it (strong +1). The
            // tag table is non-owning; the handle is owned by the field it lands
            // in via the constructor, so refcounts balance to a single disposal.
            if (const TypeSymbol* shElemTs =
                    (isGraphMode ? handledSharedElementClass(comp.ty) : nullptr)) {
                auto* shMrgBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.sh.mrg", currentFn_);
                // §9.8 v0.141 / #703 slice S4: a nullable Shared<C>? whose wire
                // value is the null marker reads back as a null handle.
                llvm::BasicBlock* shNullEndBB = nullptr;
                if (comp.ty->nullable) {
                    auto* tryNullFn =
                        declareC("bz_binder_try_read_null", i8(), {ptrTy()});
                    llvm::Value* wasNull = builder_.CreateCall(
                        tryNullFn, {derPtr}, "cdeser.sh.wasnull");
                    llvm::Value* wasNullB = builder_.CreateICmpNE(
                        wasNull, builder_.getInt8(0), "cdeser.sh.isnull");
                    auto* shNullBB = llvm::BasicBlock::Create(
                        llvm_, "cdeser.sh.null", currentFn_);
                    auto* shNotNullBB = llvm::BasicBlock::Create(
                        llvm_, "cdeser.sh.notnull", currentFn_);
                    builder_.CreateCondBr(wasNullB, shNullBB, shNotNullBB);
                    builder_.SetInsertPoint(shNullBB);
                    shNullEndBB = builder_.GetInsertBlock();
                    builder_.CreateBr(shMrgBB);
                    builder_.SetInsertPoint(shNotNullBB);
                }
                llvm::AllocaInst* tagSlot = entryAlloca(i32());
                builder_.CreateStore(builder_.getInt32(0), tagSlot);
                auto* readMarkerFn = declareC("bz_binder_read_graph_marker",
                                              i32(), {ptrTy(), ptrTy()});
                llvm::Value* marker = builder_.CreateCall(
                    readMarkerFn, {derPtr, tagSlot}, "cdeser.sh.mk");
                llvm::Value* tagId =
                    builder_.CreateLoad(i32(), tagSlot, "cdeser.sh.tag");
                // BZ_SER_INLINE = 0x01, BZ_SER_REF = 0x02 (runtime constants).
                llvm::Value* isInline = builder_.CreateICmpEQ(
                    marker, builder_.getInt32(1), "cdeser.sh.isinline");
                auto* shInlBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.sh.inl", currentFn_);
                auto* shRefBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.sh.ref", currentFn_);
                builder_.CreateCondBr(isInline, shInlBB, shRefBB);
                // Inline (slice S5, cyclic-safe): create a PLACEHOLDER handle
                // (value=null) and register it under the tag BEFORE constructing
                // the element body, so a Weak back-reference (or Shared alias)
                // encountered while building that body resolves and downgrades/
                // clones THIS handle (closing cycles). Then construct the body and
                // fill the value (bz_shared_set_value). For acyclic data this is
                // equivalent to the earlier construct-then-wrap order.
                builder_.SetInsertPoint(shInlBB);
                auto* newShFn =
                    declareC("bz_shared_new", ptrTy(), {ptrTy(), ptrTy()});
                llvm::Value* dispArg =
                    llvm::cast<llvm::Value>(elementDisposerThunk());
                llvm::Value* inlHandle = builder_.CreateCall(
                    newShFn,
                    {llvm::ConstantPointerNull::get(ptrTy()), dispArg},
                    "cdeser.sh.placeholder");
                auto* regFn = declareC("bz_binder_graph_register_tag",
                                       voidTy, {ptrTy(), i32(), ptrTy()});
                builder_.CreateCall(regFn, {derPtr, tagId, inlHandle});
                llvm::Value* childVal = emitClassDeserialize(shElemTs, derPtr);
                auto* setValFn = declareC("bz_shared_set_value", voidTy,
                                          {ptrTy(), ptrTy()});
                builder_.CreateCall(setValFn, {inlHandle, childVal});
                llvm::BasicBlock* inlEndBB = builder_.GetInsertBlock();
                builder_.CreateBr(shMrgBB);
                // Ref: resolve handle (null → cycle/unknown → throw), clone it.
                builder_.SetInsertPoint(shRefBB);
                auto* resolveFn = declareC("bz_binder_graph_resolve_tag",
                                           ptrTy(), {ptrTy(), i32()});
                llvm::Value* resolved = builder_.CreateCall(
                    resolveFn, {derPtr, tagId}, "cdeser.sh.res");
                llvm::Value* isNullR = builder_.CreateICmpEQ(
                    resolved, llvm::ConstantPointerNull::get(ptrTy()),
                    "cdeser.sh.resnull");
                auto* shCycBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.sh.cyc", currentFn_);
                auto* shOkBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.sh.ok", currentFn_);
                builder_.CreateCondBr(isNullR, shCycBB, shOkBB);
                builder_.SetInsertPoint(shCycBB);
                emitThrowBuiltin("SerializationException");
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateUnreachable();
                builder_.SetInsertPoint(shOkBB);
                auto* cloneFn =
                    declareC("bz_shared_clone", ptrTy(), {ptrTy()});
                llvm::Value* refHandle = builder_.CreateCall(
                    cloneFn, {resolved}, "cdeser.sh.clone");
                llvm::BasicBlock* refEndBB = builder_.GetInsertBlock();
                builder_.CreateBr(shMrgBB);
                // Merge: fv = phi(inline handle | cloned handle | null).
                builder_.SetInsertPoint(shMrgBB);
                llvm::PHINode* shPhi = builder_.CreatePHI(
                    ptrTy(), shNullEndBB ? 3 : 2, "cdeser.sh.fv");
                shPhi->addIncoming(inlHandle, inlEndBB);
                shPhi->addIncoming(refHandle, refEndBB);
                if (shNullEndBB)
                    shPhi->addIncoming(
                        llvm::ConstantPointerNull::get(ptrTy()), shNullEndBB);
                fv = shPhi;
            } else if (const TypeSymbol* wElemTs =
                           (isGraphMode && comp.isOwned)
                               ? handledWeakElementClass(comp.ty) : nullptr) {
                // §9.8 v0.141 / #703 slice S3: owned Weak<C> back-reference.
                // A null marker → null weak. A $ref → resolve the participant
                // handle and bz_shared_downgrade it (weak count +1; the field's
                // disposal releases it via bz_weak_release). A null resolve means
                // the tag is in-progress (a cycle) or unknown — not handled by
                // the DAG slice → SerializationException.
                (void)wElemTs;
                auto* tryNullFn = declareC("bz_binder_try_read_null", i8(), {ptrTy()});
                llvm::Value* wasNull =
                    builder_.CreateCall(tryNullFn, {derPtr}, "cdeser.wk.wasnull");
                llvm::Value* wasNullB = builder_.CreateICmpNE(
                    wasNull, builder_.getInt8(0), "cdeser.wk.isnull");
                auto* wkNullBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.wk.null", currentFn_);
                auto* wkRefBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.wk.ref", currentFn_);
                auto* wkMrgBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.wk.mrg", currentFn_);
                builder_.CreateCondBr(wasNullB, wkNullBB, wkRefBB);
                builder_.SetInsertPoint(wkNullBB);
                llvm::BasicBlock* wkNullEnd = builder_.GetInsertBlock();
                builder_.CreateBr(wkMrgBB);
                builder_.SetInsertPoint(wkRefBB);
                llvm::AllocaInst* wkTagSlot = entryAlloca(i32());
                builder_.CreateStore(builder_.getInt32(0), wkTagSlot);
                auto* readMarkerFn = declareC("bz_binder_read_graph_marker",
                                              i32(), {ptrTy(), ptrTy()});
                builder_.CreateCall(readMarkerFn, {derPtr, wkTagSlot});
                llvm::Value* wkTag =
                    builder_.CreateLoad(i32(), wkTagSlot, "cdeser.wk.tag");
                auto* wkResolveFn = declareC("bz_binder_graph_resolve_tag",
                                             ptrTy(), {ptrTy(), i32()});
                llvm::Value* wkResolved = builder_.CreateCall(
                    wkResolveFn, {derPtr, wkTag}, "cdeser.wk.res");
                llvm::Value* wkNullR = builder_.CreateICmpEQ(
                    wkResolved, llvm::ConstantPointerNull::get(ptrTy()),
                    "cdeser.wk.resnull");
                auto* wkCycBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.wk.cyc", currentFn_);
                auto* wkOkBB =
                    llvm::BasicBlock::Create(llvm_, "cdeser.wk.ok", currentFn_);
                builder_.CreateCondBr(wkNullR, wkCycBB, wkOkBB);
                builder_.SetInsertPoint(wkCycBB);
                emitThrowBuiltin("SerializationException");
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateUnreachable();
                builder_.SetInsertPoint(wkOkBB);
                auto* downFn =
                    declareC("bz_shared_downgrade", ptrTy(), {ptrTy()});
                llvm::Value* wkHandle = builder_.CreateCall(
                    downFn, {wkResolved}, "cdeser.wk.down");
                llvm::BasicBlock* wkOkEnd = builder_.GetInsertBlock();
                builder_.CreateBr(wkMrgBB);
                builder_.SetInsertPoint(wkMrgBB);
                llvm::PHINode* wkPhi =
                    builder_.CreatePHI(ptrTy(), 2, "cdeser.wk.fv");
                wkPhi->addIncoming(llvm::ConstantPointerNull::get(ptrTy()),
                                   wkNullEnd);
                wkPhi->addIncoming(wkHandle, wkOkEnd);
                fv = wkPhi;
            } else if (isGraphMode && comp.isBackEdge && isClassComp) {
                // §9.8 tier 3: back-edge field.
                // The wire format for a back-edge is one of:
                //   0x00                       → nullable back-edge was null
                //   BZ_SER_REF (0x02) + LE32   → non-null ref to already-built object
                // First try to consume a null marker (0x00 byte).  bz_binder_try_read_null
                // peeks: if the next byte is 0x00, it consumes it and returns 1 (int8).
                // If not 0x00, it returns 0 without consuming anything.
                auto* tryNullFn = declareC("bz_binder_try_read_null", i8(), {ptrTy()});
                llvm::Value* wasNull = builder_.CreateCall(tryNullFn, {derPtr}, "cdeser.be.wasnull");
                llvm::Value* wasNullBool = builder_.CreateICmpNE(
                    wasNull, builder_.getInt8(0), "cdeser.be.isnull");
                auto* beNullBB    = llvm::BasicBlock::Create(llvm_, "cdeser.be.null",     currentFn_);
                auto* beRefBB     = llvm::BasicBlock::Create(llvm_, "cdeser.be.ref",      currentFn_);
                auto* beResolvBB  = llvm::BasicBlock::Create(llvm_, "cdeser.be.resolv",   currentFn_);
                auto* beMergeBB   = llvm::BasicBlock::Create(llvm_, "cdeser.be.merge",    currentFn_);
                builder_.CreateCondBr(wasNullBool, beNullBB, beRefBB);
                // Null path: back-edge was null on serialize → fv = null ptr.
                builder_.SetInsertPoint(beNullBB);
                builder_.CreateBr(beMergeBB);
                // Ref path: read BZ_SER_REF (0x02) + LE32 tag_id, resolve.
                builder_.SetInsertPoint(beRefBB);
                llvm::AllocaInst* tagSlot = entryAlloca(i32());
                builder_.CreateStore(builder_.getInt32(0), tagSlot);
                auto* readMarkerFn = declareC("bz_binder_read_graph_marker",
                                              i32(), {ptrTy(), ptrTy()});
                // (marker result not checked — trust wire format is REF here)
                builder_.CreateCall(readMarkerFn, {derPtr, tagSlot});
                llvm::Value* tagId = builder_.CreateLoad(i32(), tagSlot, "cdeser.be.tag");
                auto* resolveFn = declareC("bz_binder_graph_resolve_tag",
                                           ptrTy(), {ptrTy(), i32()});
                llvm::Value* resolvedPtr = builder_.CreateCall(
                    resolveFn, {derPtr, tagId}, "cdeser.be.ptr");
                // Null resolve result means cycle (in-progress) or unknown tag → raise.
                llvm::Value* isNullPtr = builder_.CreateICmpEQ(
                    resolvedPtr, llvm::ConstantPointerNull::get(ptrTy()),
                    "cdeser.be.resolnull");
                auto* cycleBB    = llvm::BasicBlock::Create(llvm_, "cdeser.cycle",    currentFn_);
                builder_.CreateCondBr(isNullPtr, cycleBB, beResolvBB);
                builder_.SetInsertPoint(cycleBB);
                emitThrowBuiltin("SerializationException");
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateUnreachable();
                builder_.SetInsertPoint(beResolvBB);
                builder_.CreateBr(beMergeBB);
                // Merge: fv = phi(null | resolvedPtr).
                builder_.SetInsertPoint(beMergeBB);
                llvm::PHINode* bePhi = builder_.CreatePHI(ptrTy(), 2, "cdeser.be.fv");
                bePhi->addIncoming(llvm::ConstantPointerNull::get(ptrTy()), beNullBB);
                bePhi->addIncoming(resolvedPtr, beResolvBB);
                fv = bePhi;
            } else if (isGraphMode && comp.isOwned && isClassComp) {
                // §9.8 tier 3: owned class-typed component in graph mode.
                // Read BZ_SER_INLINE marker + tag_id, mark in-progress, deserialize
                // body (which builds the object), register tag.
                llvm::AllocaInst* tagSlot = entryAlloca(i32());
                builder_.CreateStore(builder_.getInt32(0), tagSlot);
                auto* readMarkerFn = declareC("bz_binder_read_graph_marker",
                                              i32(), {ptrTy(), ptrTy()});
                builder_.CreateCall(readMarkerFn, {derPtr, tagSlot});
                llvm::Value* tagId = builder_.CreateLoad(i32(), tagSlot, "cdeser.ot.tag");
                // Mark the tag as in-progress (cycle sentinel).
                auto* markFn = declareC("bz_binder_graph_mark_in_progress",
                                        voidTy, {ptrTy(), i32()});
                builder_.CreateCall(markFn, {derPtr, tagId});
                // §9.8 tier 4: deserialize the inline object body; if the component
                // type has a polymorphic registry, dispatch via "$type" discriminator.
                const TypeSymbol* compTs = comp.ty->declaration;
                auto* compTd = dynamic_cast<const ast::TypeDecl*>(compTs->decl);
                llvm::Value* childPtr =
                    (compTd && compTd->serializedClass &&
                     !compTd->serializedClass->polyCases.empty())
                    ? emitPolyClassDeserialize(compTs, derPtr)
                    : emitClassDeserialize(compTs, derPtr);
                // Register the constructed object with the tag.
                auto* regFn = declareC("bz_binder_graph_register_tag",
                                       voidTy, {ptrTy(), i32(), ptrTy()});
                builder_.CreateCall(regFn, {derPtr, tagId, childPtr});
                fv = childPtr;
            } else {
                // Tree mode (or value types, strings, records): existing path.
                fv = emitDeserializeField(comp.ty, derPtr);
            }

            if (fv && slots[ci]) {
                llvm::Type* lt = lowerTypeRef(comp.ty);
                if (lt && fv->getType() == lt)
                    builder_.CreateStore(fv, slots[ci]);
                else if (fv->getType()->isPointerTy() && lt && lt->isPointerTy())
                    builder_.CreateStore(fv, slots[ci]);
            }
            // fix #668: mark this required component as seen.
            for (const auto& se : seenFlags) {
                if (se.compIdx == ci) {
                    builder_.CreateStore(builder_.getInt8(1), se.slot);
                    break;
                }
            }
            auto* relFn = declareC("bz_string_release", voidTy, {ptrTy()});
            builder_.CreateCall(relFn, {namePtr});
            // Null the slot so the stringRelease disposal doesn't double-release.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), namePtrSlot);
        }
        builder_.CreateBr(afterBB);

        builder_.SetInsertPoint(nextBB);
    }

    // No match → SkipValue.
    builder_.CreateBr(skipBB);
    builder_.SetInsertPoint(skipBB);
    {
        auto* skipFn = declareC("bz_binder_skip_value", voidTy, {ptrTy()});
        builder_.CreateCall(skipFn, {derPtr});
        auto* relFn  = declareC("bz_string_release", voidTy, {ptrTy()});
        builder_.CreateCall(relFn, {namePtr});
        // Null the slot so the stringRelease disposal doesn't double-release.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), namePtrSlot);
    }
    builder_.CreateBr(afterBB);

    builder_.SetInsertPoint(afterBB);
    {
        llvm::Value* newIdx = builder_.CreateAdd(idx, builder_.getInt32(1), "cdeser.i1");
        builder_.CreateStore(newIdx, idxSlot);
    }
    builder_.CreateBr(loopHdr);

    builder_.SetInsertPoint(loopEnd);

    // EndObject.
    auto* endFn = declareC("bz_binder_end_object", voidTy, {ptrTy()});
    builder_.CreateCall(endFn, {derPtr});

    // fix #668: §9.8 / §2.15: check that all required (non-skip, non-optional)
    // components were present in the wire data.  Any component whose seen-flag
    // is still 0 means the field was absent → SerializationException.
    // Checked after EndObject so the deserializer has consumed the full object
    // token, but before calloc so no allocation cleanup is needed on this path.
    for (const auto& se : seenFlags) {
        llvm::Value* seen = builder_.CreateLoad(i8(), se.slot, "cdeser.seen");
        llvm::Value* notSeen = builder_.CreateICmpEQ(seen, builder_.getInt8(0), "cdeser.nseen");
        auto* missBB = llvm::BasicBlock::Create(llvm_, "cdeser.miss", currentFn_);
        auto* okBB   = llvm::BasicBlock::Create(llvm_, "cdeser.ok",   currentFn_);
        builder_.CreateCondBr(notSeen, missBB, okBB);
        builder_.SetInsertPoint(missBB);
        // Dispose already-read owned-class slots to avoid leaks (§28.3/§28.5).
        for (std::size_t ci2 = 0; ci2 < comps.size(); ++ci2) {
            if (comps[ci2].isSkip || !slots[ci2]) continue;
            if (!comps[ci2].ty) continue;
            bool isClassSlot =
                comps[ci2].ty->kind == sema::TypeRef::Kind::Named &&
                comps[ci2].ty->declaration &&
                comps[ci2].ty->declaration->kind == ast::TypeDeclKind::Class;
            if (!isClassSlot) continue;
            llvm::Value* p = builder_.CreateLoad(ptrTy(), slots[ci2], "cdeser.mslot");
            llvm::Value* nonNull = builder_.CreateICmpNE(
                p, llvm::ConstantPointerNull::get(ptrTy()), "cdeser.mnn");
            auto* dispBB2 = llvm::BasicBlock::Create(llvm_, "cdeser.mdisp", currentFn_);
            auto* skipBB3 = llvm::BasicBlock::Create(llvm_, "cdeser.mskip", currentFn_);
            builder_.CreateCondBr(nonNull, dispBB2, skipBB3);
            builder_.SetInsertPoint(dispBB2);
            emitDisposeAndReclaim(p);
            builder_.CreateBr(skipBB3);
            builder_.SetInsertPoint(skipBB3);
        }
        emitThrowBuiltin("SerializationException");
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateUnreachable();
        builder_.SetInsertPoint(okBB);
    }

    // Allocate the class instance.
    llvm::Value* sizeofSt = builder_.CreatePtrToInt(
        builder_.CreateGEP(st, llvm::ConstantPointerNull::get(ptrTy()),
                           {builder_.getInt32(1)}),
        i64());
    llvm::Value* obj = builder_.CreateCall(callocFn(), {builder_.getInt64(1), sizeofSt});

    // Run field initializers (emitInstanceInit).
    {
        llvm::Value* savedThis = currentThis_;
        currentThis_ = obj;
        emitInstanceInit(ts, obj, st);
        currentThis_ = savedThis;
    }

    // §9.8 v0.136: find the unique constructor whose parameter names match
    // exactly the set of serialized component declared identifiers
    // (bijection, order-insensitive).  `serialized "<name>"` renames do not
    // affect the match — which uses declared identifiers only (§9.8 v0.136).
    // Once found, arguments are passed IN CONSTRUCTOR PARAMETER ORDER, with
    // each parameter value looked up by its name in the component map.
    //
    // Build a map from lowercased component declName → component index.
    // §9.8 v0.136: the match is case-insensitive: the Bezel convention uses
    // PascalCase for field/property declared identifiers and camelCase for
    // constructor parameters.  The spec says "matched by name" using declared
    // identifiers, and the idiomatic convention means `id` binds `Id`, `m`
    // binds `M`, etc.  We normalise to lowercase for the map key so that
    // `tag` and `Tag` resolve to the same component.
    std::unordered_map<std::string, std::size_t> compByNameLower;
    for (std::size_t ci = 0; ci < comps.size(); ++ci)
        compByNameLower[toLowerAscii(comps[ci].declName)] = ci;

    sema::ConstructorSymbol* matchedCtor = nullptr;
    for (Symbol* m : ts->members) {
        auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
        if (!cs || cs->isStatic) continue;
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) continue;
        // Match: param count == component count AND every param name (lowered)
        // maps to a distinct component (bijection).
        if (cd->params.size() != comps.size()) continue;
        bool allMatch = true;
        for (const auto& p : cd->params) {
            if (compByNameLower.find(toLowerAscii(p.name)) ==
                    compByNameLower.end()) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) {
            matchedCtor = cs;
            break;
        }
    }

    if (matchedCtor) {
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(matchedCtor->decl);
        auto it  = ctorFns_.find(matchedCtor);
        if (it != ctorFns_.end() && cd) {
            // Build the argument list IN CONSTRUCTOR PARAMETER ORDER.
            // For each ctor param, look up the corresponding component by
            // lowercased param name (case-insensitive match to component
            // declName).  Non-skip component → deserialized value from slot.
            // Skip component → default(T) (zero/null).
            std::vector<llvm::Value*> args;
            args.push_back(obj);  // `this`

            auto* fnTy = it->second->getFunctionType();
            bool  ok   = true;
            for (std::size_t pi = 0; pi < cd->params.size(); ++pi) {
                // Find the component for this parameter by lowercased name.
                auto cit = compByNameLower.find(
                    toLowerAscii(cd->params[pi].name));
                if (cit == compByNameLower.end()) { ok = false; break; }
                std::size_t ci = cit->second;
                if (!comps[ci].ty) { ok = false; break; }
                llvm::Type* lt = lowerTypeRef(comps[ci].ty);
                if (!lt) { ok = false; break; }
                llvm::Value* fv;
                if (comps[ci].isSkip || !slots[ci]) {
                    // Skip component or unresolved slot → default(T).
                    fv = llvm::Constant::getNullValue(lt);
                } else {
                    fv = builder_.CreateLoad(lt, slots[ci], "cdeser.arg");
                }
                // Coerce to ctor param LLVM type if needed.
                if (pi + 1 < fnTy->getNumParams())
                    fv = coerceNumericToType(fv, fnTy->getParamType(pi + 1), false);
                args.push_back(fv);
            }
            if (ok && args.size() == it->second->arg_size()) {
                builder_.CreateCall(it->second, args);
                emitNewPendingCheck(obj);
                // fix #671 / §4.6 / §28.9: the ctor body stores each string
                // parameter into its corresponding auto-property field via
                // lowerStringFieldStore, which RETAINS the value (the param is
                // not a +1 producer, so a fresh +1 is minted for the field).
                // bz_binder_read_string returned rc=1; after the ctor's retain
                // the field holds a +1 reference (rc=2) and the slot's original
                // reference (rc=1) is the binder's still-live extra.  Release
                // that binder reference by reading back from the FIELD after the
                // ctor — so we observe exactly the retained ptr — and calling
                // bz_string_release once.  The disposer's later release then
                // correctly brings rc to 0.
                // Only string components carry a runtime-managed rc; skip all
                // other value types (int, bool, double, decimal, enum, record).
                llvm::Type* voidTy2 = llvm::Type::getVoidTy(llvm_);
                auto* relFn = declareC("bz_string_release", voidTy2, {ptrTy()});
                for (std::size_t pi = 0; pi < cd->params.size(); ++pi) {
                    auto cit2 = compByNameLower.find(
                        toLowerAscii(cd->params[pi].name));
                    if (cit2 == compByNameLower.end()) continue;
                    std::size_t ci2 = cit2->second;
                    if (comps[ci2].isSkip || !slots[ci2]) continue;
                    if (!comps[ci2].ty) continue;
                    if (comps[ci2].ty->kind != sema::TypeRef::Kind::Primitive ||
                        comps[ci2].ty->primitive != sema::PrimitiveType::String)
                        continue;
                    // Read the string ptr from the FIELD (not the pre-ctor
                    // slot) so we observe exactly what the ctor wrote and
                    // retained.  The field ptr is the same as the slot ptr
                    // (ctor stores the incoming arg), but reading from the field
                    // is the canonical post-ctor source of truth.
                    unsigned fIdx = comps[ci2].fieldIdx;
                    llvm::Value* fAddr = builder_.CreateStructGEP(
                        st, obj, fIdx, "cdeser.faddr");
                    llvm::Value* sp = builder_.CreateLoad(
                        ptrTy(), fAddr, "cdeser.frel");
                    // Null-guard (defensive — the ctor should always set the
                    // field, but if it fails to do so a null release is harmless
                    // since bz_string_release is null-safe).
                    llvm::Value* nonNull = builder_.CreateICmpNE(
                        sp, llvm::ConstantPointerNull::get(ptrTy()),
                        "cdeser.fnn");
                    auto* relBB  = llvm::BasicBlock::Create(
                        llvm_, "cdeser.fstr.rel", currentFn_);
                    auto* skipBB = llvm::BasicBlock::Create(
                        llvm_, "cdeser.fstr.skip", currentFn_);
                    builder_.CreateCondBr(nonNull, relBB, skipBB);
                    builder_.SetInsertPoint(relBB);
                    builder_.CreateCall(relFn, {sp});
                    builder_.CreateBr(skipBB);
                    builder_.SetInsertPoint(skipBB);
                }
            }
        }
    }

    return obj;
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

    // BinarySerializer / BinaryDeserializer / ISerializer / IDeserializer
    // instance methods.
    auto* recvTy = ctx_.exprType(*ma.target);
    if (recvTy && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty()) {
        const std::string_view rcn = recvTy->astNode->name->parts[0];
        if (rcn == "BinarySerializer" || rcn == "BinaryDeserializer") return true;
        // §2.15: JsonSerializer / JsonDeserializer instance methods.
        if (rcn == "JsonSerializer" || rcn == "JsonDeserializer") return true;
        // §2.14: ISerializer / IDeserializer methods (used in hand-written
        // Serialize/Deserialize bodies; the concrete object is always a
        // BinarySerializer* / BinaryDeserializer* at runtime).
        if (rcn == "ISerializer" || rcn == "IDeserializer") return true;
        // §4.14 / #636: OwnedBuffer<T>.AsSpan() / AsReadOnlySpan() / Count /
        // Length are no longer intercepted here.  They are now handled by:
        //   - AsSpan / AsReadOnlySpan: tryLowerArraySpanCall (same path as
        //     Array<T>, delegated so both share one element-width–correct path)
        //   - Count / Length: tryLowerCollectionInstanceCall (bz_array_len)
        // Removing this block eliminates the duplicate serialization-handler
        // path that lacked bounds checks and element-width handling.
    }

    // Synthesized Serialize on record/enum/class.
    if (mn == "Serialize" && call.args.size() == 1) {
        auto* recvTy2 = ctx_.exprType(*ma.target);
        if (recvTy2 && recvTy2->declaration) {
            const TypeSymbol* ts = recvTy2->declaration;
            if ((ts->kind == ast::TypeDeclKind::Record ||
                 ts->kind == ast::TypeDeclKind::Enum) &&
                declaresConformance(ts, "ISerializable") &&
                !hasHandWrittenMethod(ts, "Serialize"))
                return true;
            // §9.8 tier 2: synthesized Serialize on a class with serialized clause.
            if (ts->kind == ast::TypeDeclKind::Class &&
                declaresConformance(ts, "ISerializable") &&
                !hasHandWrittenMethod(ts, "Serialize")) {
                auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
                if (td && td->serializedClass) return true;
            }
        }
    }

    // Synthesized static Deserialize on record/enum/class type name.
    if (mn == "Deserialize" && call.args.size() == 1) {
        const sema::Symbol* sym = ctx_.exprSymbol(*ma.target);
        if (auto* ts = dynamic_cast<const TypeSymbol*>(sym)) {
            if ((ts->kind == ast::TypeDeclKind::Record ||
                 ts->kind == ast::TypeDeclKind::Enum) &&
                declaresConformance(ts, "IDeserializable") &&
                !hasHandWrittenMethod(ts, "Deserialize"))
                return true;
            // §9.8 tier 2: synthesized Deserialize on a class with serialized clause.
            if (ts->kind == ast::TypeDeclKind::Class &&
                declaresConformance(ts, "IDeserializable") &&
                !hasHandWrittenMethod(ts, "Deserialize")) {
                auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
                if (td && td->serializedClass) return true;
            }
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

        // §2.15: JsonSerializer instance methods.
        if (rcn == "JsonSerializer") {
            llvm::Value* ser = lowerExpr(*ma->target);
            if (!ser) return nullptr;

            // ToBytes() → bz_jser_to_bytes(ser) returning bz_array_t*
            if (methodName == "ToBytes" && call.args.empty()) {
                auto* fn = declareC("bz_jser_to_bytes", ptrTy(), {ptrTy()});
                return builder_.CreateCall(fn, {ser}, "jser.bytes");
            }

            // ToString() → bz_jser_to_string(ser) returning bz_string_t*
            if (methodName == "ToString" && call.args.empty()) {
                auto* fn = declareC("bz_jser_to_string", ptrTy(), {ptrTy()});
                return builder_.CreateCall(fn, {ser}, "jser.str");
            }

            // Dispose() → bz_binser_free(ser)
            if (methodName == "Dispose" && call.args.empty()) {
                auto* fn = declareC("bz_binser_free",
                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {ser});
                return nullptr;
            }
            return nullptr;
        }

        // §2.15: JsonDeserializer instance methods.
        if (rcn == "JsonDeserializer") {
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

        // §2.14: ISerializer — forwarded to bz_binser_* functions.
        // The concrete object is always a BinarySerializer* at runtime.
        if (rcn == "ISerializer") {
            llvm::Value* ser = lowerExpr(*ma->target);
            if (!ser) return nullptr;
            auto voidTy = llvm::Type::getVoidTy(llvm_);

            if (methodName == "BeginObject" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* cnt = lowerExpr(*call.args[0].value);
                if (!cnt) return nullptr;
                cnt = builder_.CreateSExtOrTrunc(cnt, i32(), "begin.cnt");
                auto* fn = declareC("bz_binser_begin_object", voidTy, {ptrTy(), i32()});
                builder_.CreateCall(fn, {ser, cnt});
                return nullptr;
            }
            if (methodName == "EndObject" && call.args.empty()) {
                auto* fn = declareC("bz_binser_end_object", voidTy, {ptrTy()});
                builder_.CreateCall(fn, {ser});
                return nullptr;
            }
            if (methodName == "WriteFieldName" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* nm = lowerExpr(*call.args[0].value);
                if (!nm) return nullptr;
                auto* fn = declareC("bz_binser_write_field_name", voidTy, {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {ser, nm});
                return nullptr;
            }
            if (methodName == "WriteInt" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* v = lowerExpr(*call.args[0].value);
                if (!v) return nullptr;
                v = builder_.CreateSExtOrTrunc(v, i64(), "wrint.v");
                auto* fn = declareC("bz_binser_write_int", voidTy, {ptrTy(), i64()});
                builder_.CreateCall(fn, {ser, v});
                return nullptr;
            }
            if (methodName == "WriteUInt" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* v = lowerExpr(*call.args[0].value);
                if (!v) return nullptr;
                v = builder_.CreateZExtOrTrunc(v, i64(), "wruint.v");
                auto* fn = declareC("bz_binser_write_uint", voidTy, {ptrTy(), i64()});
                builder_.CreateCall(fn, {ser, v});
                return nullptr;
            }
            if (methodName == "WriteFloat" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* v = lowerExpr(*call.args[0].value);
                if (!v) return nullptr;
                if (!v->getType()->isDoubleTy())
                    v = builder_.CreateFPExt(v, llvm::Type::getDoubleTy(llvm_), "wrflt.v");
                auto* fn = declareC("bz_binser_write_float", voidTy,
                    {ptrTy(), llvm::Type::getDoubleTy(llvm_)});
                builder_.CreateCall(fn, {ser, v});
                return nullptr;
            }
            if (methodName == "WriteBool" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* v = lowerExpr(*call.args[0].value);
                if (!v) return nullptr;
                v = builder_.CreateZExtOrTrunc(v, i8(), "wrbool.v");
                auto* fn = declareC("bz_binser_write_bool", voidTy, {ptrTy(), i8()});
                builder_.CreateCall(fn, {ser, v});
                return nullptr;
            }
            if (methodName == "WriteString" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* v = lowerExpr(*call.args[0].value);
                if (!v) return nullptr;
                auto* fn = declareC("bz_binser_write_string", voidTy, {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {ser, v});
                return nullptr;
            }
            if (methodName == "WriteChar" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* v = lowerExpr(*call.args[0].value);
                if (!v) return nullptr;
                v = builder_.CreateSExtOrTrunc(v, i32(), "wrchar.v");
                auto* fn = declareC("bz_binser_write_char", voidTy, {ptrTy(), i32()});
                builder_.CreateCall(fn, {ser, v});
                return nullptr;
            }
            if (methodName == "WriteDecimal" && call.args.size() == 1 && call.args[0].value) {
                // §2.14 / #665: ISerializer.WriteDecimal — pass the 16-byte decimal
                // struct by pointer (4×u32 LE wire format).
                llvm::Value* v = lowerExpr(*call.args[0].value);
                if (!v) return nullptr;
                llvm::AllocaInst* decSlot = entryAlloca(decimalStructType());
                if (v->getType() == decimalStructType())
                    builder_.CreateStore(v, decSlot);
                auto* fn = declareC("bz_binser_write_decimal", voidTy,
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {ser, decSlot});
                return nullptr;
            }
            if (methodName == "WriteNull" && call.args.empty()) {
                auto* fn = declareC("bz_binser_write_null", voidTy, {ptrTy()});
                builder_.CreateCall(fn, {ser});
                return nullptr;
            }
            if (methodName == "BeginArray" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* cnt = lowerExpr(*call.args[0].value);
                if (!cnt) return nullptr;
                cnt = builder_.CreateSExtOrTrunc(cnt, i32(), "bgarr.cnt");
                auto* fn = declareC("bz_binser_begin_array", voidTy, {ptrTy(), i32()});
                builder_.CreateCall(fn, {ser, cnt});
                return nullptr;
            }
            if (methodName == "EndArray" && call.args.empty()) {
                auto* fn = declareC("bz_binser_end_array", voidTy, {ptrTy()});
                builder_.CreateCall(fn, {ser});
                return nullptr;
            }
            return nullptr;
        }

        // §2.14: IDeserializer — forwarded to bz_binder_* functions.
        if (rcn == "IDeserializer") {
            llvm::Value* der = lowerExpr(*ma->target);
            if (!der) return nullptr;
            // IDeserializer methods not yet needed in hand-written Deserialize
            // bodies (no test exercises this path in tier 2). Return nullptr to
            // fall through gracefully.
            (void)der;
            return nullptr;
        }

        // §4.14 / #636: OwnedBuffer<T>.AsSpan() / AsReadOnlySpan() / Count /
        // Length are no longer handled here — they are delegated to:
        //   - AsSpan / AsReadOnlySpan: tryLowerArraySpanCall (same lowering
        //     path as Array<T>; handles all element types with correct
        //     element-width and bounds checks)
        //   - Count / Length: tryLowerCollectionInstanceCall (bz_array_len)
        // Since isSerializationCall no longer flags these calls, this branch
        // is never reached for OwnedBuffer/Array AsSpan/Count/Length.
        // The duplicate path has been removed to keep a single lowering path.
    }

    // -----------------------------------------------------------------------
    // Synthesized Serialize(s: ISerializer) on a record, enum, or class.
    // -----------------------------------------------------------------------
    if (methodName == "Serialize" && call.args.size() == 1 && call.args[0].value) {
        auto* recvTy2 = ctx_.exprType(*ma->target);
        if (recvTy2 && recvTy2->kind == sema::TypeRef::Kind::Named &&
            recvTy2->declaration) {
            const TypeSymbol* ts = recvTy2->declaration;
            bool isRecord = ts->kind == ast::TypeDeclKind::Record;
            bool isEnum   = ts->kind == ast::TypeDeclKind::Enum;
            bool isClass  = ts->kind == ast::TypeDeclKind::Class;
            if ((isRecord || isEnum || isClass) &&
                declaresConformance(ts, "ISerializable") &&
                !hasHandWrittenMethod(ts, "Serialize")) {

                if (isClass) {
                    auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
                    if (!td || !td->serializedClass) return nullptr;
                }

                llvm::Value* serArg = lowerExpr(*call.args[0].value);
                if (!serArg) return nullptr;
                llvm::Value* recv = lowerExpr(*ma->target);
                if (!recv) return nullptr;

                if (isRecord) {
                    llvm::StructType* st = recordStruct(ts);
                    if (recv->getType()->isPointerTy())
                        recv = builder_.CreateLoad(st, recv);
                    emitRecordSerialize(ts, recv, serArg);
                } else if (isEnum) {
                    if (!recv->getType()->isIntegerTy(32))
                        recv = builder_.CreateSExtOrTrunc(recv, i32(), "enum.v");
                    emitEnumSerialize(ts, recv, serArg);
                } else {
                    // Class: recv is a ptr to the class instance.
                    // §9.8 tier 4: polymorphic base → poly dispatch.
                    auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
                    if (td && td->serializedClass &&
                        !td->serializedClass->polyCases.empty())
                        emitPolyClassSerialize(ts, recv, serArg);
                    else
                        emitClassSerialize(ts, recv, serArg);
                }
                return nullptr;  // void method
            }
        }
    }

    // -----------------------------------------------------------------------
    // Synthesized static Deserialize(d: IDeserializer) on a record, enum, or class.
    // The callee is a MemberAccessExpr with target = IdentifierExpr(<TypeName>).
    // -----------------------------------------------------------------------
    if (methodName == "Deserialize" && call.args.size() == 1 && call.args[0].value) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (id) {
            const sema::Symbol* sym = ctx_.exprSymbol(*ma->target);
            const TypeSymbol* ts = dynamic_cast<const TypeSymbol*>(sym);
            if (ts && declaresConformance(ts, "IDeserializable") &&
                !hasHandWrittenMethod(ts, "Deserialize")) {
                if (ts->kind == ast::TypeDeclKind::Record ||
                    ts->kind == ast::TypeDeclKind::Enum) {
                    llvm::Value* derArg = lowerExpr(*call.args[0].value);
                    if (!derArg) return nullptr;
                    if (ts->kind == ast::TypeDeclKind::Record)
                        return emitRecordDeserialize(ts, derArg);
                    else
                        return emitEnumDeserialize(ts, derArg);
                }
                if (ts->kind == ast::TypeDeclKind::Class) {
                    auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
                    if (!td || !td->serializedClass) return nullptr;
                    llvm::Value* derArg = lowerExpr(*call.args[0].value);
                    if (!derArg) return nullptr;
                    // §9.8 tier 4: polymorphic base → poly dispatch.
                    if (!td->serializedClass->polyCases.empty())
                        return emitPolyClassDeserialize(ts, derArg);
                    return emitClassDeserialize(ts, derArg);
                }
            }
        }
    }

    return nullptr;
}

} // namespace bezel::codegen
