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
// (BinarySerializer/BinaryDeserializer), §4.14 (OwningBuffer<T>/Span<T>).
// Issue #550, tiers 1+2.

#include "codegen/LlvmBackend_impl.hpp"

#include "ast/Node.hpp"
#include "sema/EnumValues.hpp"  // fix #3140: enumConstantWireNames — §9.8
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
//
// fix #3139 (§9.8 / §2.5, family F1 of #3152): ask the ONE symbol-side walk
// (`sema::declaresBuiltinInterface`), not the AST. The `ast::TypeDecl` this
// read `ts->decl` for is null for EVERY imported type — the importing
// compilation has no AST for a dependency's types at all — so the `!ts->decl`
// early-out answered "declares no conformance" for every imported record,
// enum and class, and §9.8's whole synthesized surface silently switched off
// across a module boundary.
static bool declaresConformance(const TypeSymbol* ts, std::string_view name) {
    return sema::declaresBuiltinInterface(ts, name);
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

// The `serialized` clause on a §35.1 CHOICE-CASE PAYLOAD field.
//
// ⚠ fix #3140 (§9.8, family F2 of umbrella #3152): these two used to serve the
// RECORD path as well, and there they answered "no clause" for every imported
// component — `ast::Param::serialized` is the AST of a local record only. That
// is not a benign default: dropping a `serialized skip` writes a component the
// wire header's count excludes, desynchronising the reader (the defect #1612
// fixed on the local side), and a `serialized "<name>"` rename written under the
// declared name is reported by a name-keyed reader as a missing required field.
// The record path now reads `sema::serializedComponentClause`, which rides
// `MemberSymbol` on both sides.
//
// They survive for the CHOICE path alone: a choice case's payload fields are not
// members and have no `Symbol` of their own — they are reached through
// `ChoiceCaseSymbol::caseDecl`, an `ast::` pointer.
//
// ⚠ AN EARLIER REVISION OF THIS COMMENT SAID "nothing about an imported
// choice's case list crosses the boundary at all". That was true when it was
// written and is now FALSE: family F9 (#3144, `feec091`) landed the case list,
// and `ModuleLoader` points `caseDecl` at a faithful RECONSTRUCTION of the
// `ast::ChoiceCase` — including each payload `ast::Param` — so these two helpers
// now read a real node on both sides rather than a null one on the imported
// side. Corrected here rather than left standing, because "there is no second
// source" was the justification for keeping them, and that justification is
// gone even though the code is now MORE correct than the comment claimed.
//
// What has NOT been established is whether a payload `Param`'s `serialized`
// rename survives that reconstruction — F9 carried the case list, and this
// lane's tests exercise a payload-free imported choice only. Do NOT assume it
// does; see the sibling check in `serializationSurfaceIsEmittable`.
//
// Do NOT reintroduce these on the RECORD path — there the clause rides
// `MemberSymbol` and `sema::serializedComponentClause` is the one reader.
static std::string_view positionalFieldWireName(const ast::Param& pf) {
    if (pf.serialized && !pf.serialized->name.empty())
        return pf.serialized->name;
    return pf.name;
}

static bool positionalFieldIsSkip(const ast::Param& pf) {
    return pf.serialized && pf.serialized->isSkip;
}

// fix #3140: the wire name for an enum constant used to be read here off the
// `ast::EnumConstant` node, which an IMPORTED enum does not have. It is now
// `sema::enumConstantWireName` (sema/EnumValues.hpp), reading a symbol field
// both SymbolCollection and ModuleLoader populate.

// §9.8 choice serialization (v0.200): the `$case` discriminator value written for
// a choice case — the case's `serialized "<name>"` rename if present, else its
// declared name.  The `serialized` clause on a case admits the rename form only
// (a case is not omittable — the enum-constant rule), enforced in sema.
static std::string_view choiceCaseWireName(const sema::ChoiceCaseSymbol* cc) {
    if (cc && cc->caseDecl && cc->caseDecl->serialized &&
        !cc->caseDecl->serialized->name.empty())
        return cc->caseDecl->serialized->name;
    return cc ? cc->name : std::string_view{};
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
    // fix #3140 (§9.8, family F2 of umbrella #3152): `td` was a BARE GUARD —
    // no `td->` member was ever read in this function's body — so all it did was
    // return an EMPTY component list for every imported class, i.e. serialize it
    // to nothing. The cast is gone; the symbol is all this walk needs.
    //
    // Reachability, stated because dropping a guard deserves it: every caller
    // (emitClassSerialize / emitClassDeserialize / emitPolyClass*) is gated on
    // `sema::serializedClassShape`, which is empty for every imported class —
    // see its comment in Symbol.hpp for why that is a measured statement about
    // cross-module class serialization (#3179) and not a missing channel. So
    // this function still never runs for an imported class today; what changed
    // is that it no longer holds a SECOND, independent opinion on the question.
    if (!ts) return;

    for (const Symbol* m : storage) {
        // Find the field index.
        auto idxIt = fieldIndex.find(m);
        if (idxIt == fieldIndex.end()) continue;

        ClassComponent comp;
        comp.fieldIdx = idxIt->second;
        comp.declName = std::string(m->name);
        comp.ty       = nullptr;

        // §9.8 / #2325: a primary-constructor-parameter-derived FieldSymbol
        // (posParam != null) has `decl` set to the owning TypeDecl, not a
        // FieldDecl/PropertyDecl — see the FieldSymbol comment in Symbol.hpp
        // and the parallel sema check (TypeResolution_Checks.cpp,
        // checkSerializationEligibility). Detected up front so the dispatch
        // below can route it before falling into the "no decl" fallback.
        const auto* fsPos = dynamic_cast<const FieldSymbol*>(m);

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
            // fix #3140 (family F2): the clause comes from the SYMBOL, which
            // SymbolCollection filled from this very `pd->serialized` and
            // ModuleLoader fills from the wire — one predicate, both sources.
            const auto clause = sema::serializedComponentClause(m);
            comp.isSkip     = clause.skip;
            comp.isOptional = clause.optional;
            comp.wireName   = std::string(clause.wireName);
            if (pd->type) comp.ty = ctx.resolvedType(*pd->type);
        } else if (fsPos && fsPos->posParam) {
            // §9.8 / #2325: a primary-constructor-parameter-derived field.
            // `storage` (collectInstanceStorage) reserves a physical slot for
            // EVERY primary-ctor parameter unconditionally, whether or not it
            // is captured — so a parameter referenced only in an initializer
            // (§11.1: "parameters referenced only in initializers do not
            // generate backing fields") still reaches this branch with a
            // valid fieldIndex entry but is not a genuine field of the type.
            // Only a CAPTURED parameter (posParamBodyRef) is a real serialized
            // component — matches the parallel sema check exactly, so this
            // guard and the eligibility check never disagree.
            if (!fsPos->posParamBodyRef) continue;
            if (!fsPos->posParam->type) continue;
            // A CLASS primary-ctor parameter carries no `serialized` clause
            // (A.4 admits one on a record's PositionalField, an auto-property's
            // PropertyBody, an EnumConstant and a ChoiceCase — never on a class
            // primary-ctor parameter), so the accessor's resolved wire name is
            // the declared identifier here by construction.
            bool isOwned = fsPos->posParam->modifier == "owned";
            comp.isOwned  = isOwned;
            comp.wireName = std::string(sema::serializedComponentClause(m).wireName);
            comp.ty = ctx.resolvedType(*fsPos->posParam->type);

            // §9.8 tier 3: identify back-edges (non-owned class-typed fields),
            // mirroring the FieldDecl branch above exactly.
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
        } else {
            // Fallback (no decl — e.g. an imported field). fix #3140: the
            // clause is available here too, since it rides the symbol.
            const auto clause = sema::serializedComponentClause(m);
            comp.isSkip     = clause.skip;
            comp.isOptional = clause.optional;
            comp.wireName   = std::string(clause.wireName);
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
        name = t->headName();
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
        name = t->headName();
    else if (!t->importedName.empty())
        name = t->importedName;
    if (name != "Shared") return nullptr;
    if (t->genericArgs.empty() || !t->genericArgs[0]) return nullptr;
    const sema::TypeRef* el = t->genericArgs[0];
    if (el->kind != sema::TypeRef::Kind::Named || !el->declaration) return nullptr;
    if (el->declaration->kind != ast::TypeDeclKind::Class) return nullptr;
    // fix #3140 (family F2): the element's §9.8 surface, from the symbol.
    if (sema::serializedClassShape(el->declaration).empty()) return nullptr;
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
        name = t->headName();
    else if (!t->importedName.empty())
        name = t->importedName;
    if (name != "Weak") return nullptr;
    if (t->genericArgs.empty() || !t->genericArgs[0]) return nullptr;
    const sema::TypeRef* el = t->genericArgs[0];
    if (el->kind != sema::TypeRef::Kind::Named || !el->declaration) return nullptr;
    if (el->declaration->kind != ast::TypeDeclKind::Class) return nullptr;
    // fix #3140 (family F2): the element's §9.8 surface, from the symbol.
    if (sema::serializedClassShape(el->declaration).empty()) return nullptr;
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
    // fix #3140 (§9.8, family F2 of umbrella #3152): this opened with
    // `if (!ts || !ts->decl) return;` and then walked `ast::TypeDecl`'s
    // `positionalFields` / each auto-property's `ast::PropertyDecl::serialized`.
    // `ModuleLoader` never assigns `Symbol::decl` for an imported type, so an
    // IMPORTED record's Serialize emitted NO BYTES, with no diagnostic at any
    // severity — sema's §9.8 checks iterate `ctx_.module()->children`, LOCAL
    // types only, so nothing ever looked. MEASURED on a two-module build with
    // the classification gate opened and nothing else changed: an imported
    // `record Pt(X: int, Y: int): ISerializable, IDeserializable` round-tripped
    // to 0 for an expected 34, and a local `Wrap(P: Pt, N: int)` over it to 5
    // for an expected 345.
    if (!ts) return;
    // fix #3140 follow-up: name this type in the backstop diagnostic.
    SerializationDiagScope diagScope(this, ts);

    // The storage walk is the SAME one recordStruct lays out, so `mi` below is
    // both the component index and the struct element index. It is both-sources
    // already (#2478's `isAutoPropertyStorage`).
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(ts, storage);

    // §9.8 / #2516: for a record with a positional parameter list, the
    // serialized components are its POSITIONAL fields; a body member of a
    // combined-form record is not one. `posParam != nullptr` is the positional
    // marker on both sides (a real `ast::Param` locally, a synthetic one on
    // import), so this selects by identity rather than by the index-lockstep the
    // AST walk used — which silently mispaired a combined-form record, since
    // SymbolCollection appends the positional fields AFTER the body members.
    const bool positional = ts->hasPositionalFields;
    auto isSerializedComponent = [&](const sema::Symbol* m) {
        if (!positional) return true;
        const auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
        return fs && fs->posParam != nullptr;
    };

    llvm::Type* voidTy  = llvm::Type::getVoidTy(llvm_);

    // BeginObject(field_count)
    // per §9.8: `serialized skip` components are excluded from both the wire
    // count and the writes — the count and the write loop below MUST filter
    // identically, or the wire header declares more fields than are actually
    // written and the reader desynchronizes (fix #1612). They now read the same
    // predicate off the same symbol, which is what makes that structural
    // instead of a convention held by two copies of one rule.
    int32_t nonSkipCount = 0;
    for (const sema::Symbol* sym : storage) {
        if (!isSerializedComponent(sym)) continue;
        if (sema::serializedComponentClause(sym).skip) continue;
        ++nonSkipCount;
    }
    auto* beginFn = declareC("bz_binser_begin_object", voidTy, {ptrTy(), i32()});
    builder_.CreateCall(beginFn, {serPtr, builder_.getInt32(nonSkipCount)});
    // §2.15 / fix #687: bz_binser_begin_object can now set bz_exc (MaxDepth guard).
    // Check for pending exception so a try/catch SerializationException can catch it.
    emitPendingCheck();

    // Per-field: WriteFieldName + Write
    for (unsigned mi = 0; mi < storage.size(); ++mi) {
        const sema::Symbol* sym = storage[mi];
        if (!isSerializedComponent(sym)) continue;
        // fix #3140 (family F2): the §9.8 clause — `skip` and the wire name —
        // from the symbol, for a positional field and an auto-property alike.
        const auto clause = sema::serializedComponentClause(sym);
        if (clause.skip) continue;
        const std::string wireName(clause.wireName);

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
            // §2.15 / fix #1229: bz_binser_write_float sets bz_exc for NaN/±Infinity
            // on a JsonSerializer; propagate via emitPendingCheck so the
            // SerializationException unwinds rather than falling through to
            // EndObject and leaving bz_exc stale (which crashes the next call).
            emitPendingCheck();
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

    // §9.8 v0.228 (issue #1065): a non-owning inline array [N]T is a standard
    // sequence component, encoded exactly like a heap collection would be —
    // BeginArray(N), each element via a recursive emitSerializeField, EndArray
    // — except N is compile-time-constant (§15.1), so the element loop
    // unrolls rather than reading a runtime Count. `val` is the raw LLVM
    // ArrayType VALUE embedded in the record's struct (an inline array is a
    // value type stored inline, not a heap pointer) — the same shape
    // emitInlineArrayEquality consumes for auto-equality (§9.2).  An OWNING
    // inline array never reaches here: isSerializationEligibleType rejects it
    // (§9.8 record clause admits only "a non-owning inline array"), so a
    // `serializedClass`/record component of that shape is already a compile
    // error at the conformance declaration — this is defensive.
    if (fieldTy->kind == K::InlineArray && !fieldTy->arrayOwned &&
        fieldTy->arrayElement && val->getType()->isArrayTy()) {
        auto* arrTy = llvm::cast<llvm::ArrayType>(val->getType());
        unsigned n = static_cast<unsigned>(arrTy->getNumElements());
        auto* beginFn = declareC("bz_binser_begin_array", voidTy,
                                 {ptrTy(), i32()});
        builder_.CreateCall(beginFn, {serPtr, builder_.getInt32(n)});
        for (unsigned i = 0; i < n; ++i) {
            llvm::Value* elem = builder_.CreateExtractValue(val, {i});
            emitSerializeField(elem, fieldTy->arrayElement, serPtr);
        }
        auto* endFn = declareC("bz_binser_end_array", voidTy, {ptrTy()});
        builder_.CreateCall(endFn, {serPtr});
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

        if (fieldTs->kind == ast::TypeDeclKind::Choice) {
            // §9.8 choice serialization (v0.200, #2046): a nested value-like
            // choice component (a choice payload of a choice, or a choice-typed
            // record component) recurses into the `$case` object form.  The
            // value is a choice struct (value type); load if it is a slot ptr.
            llvm::StructType* st = choiceStruct(fieldTs);
            llvm::Value* chVal = val;
            if (chVal->getType()->isPointerTy())
                chVal = builder_.CreateLoad(st, chVal);
            emitChoiceSerialize(fieldTs, chVal, serPtr);
            return;
        }

        if (fieldTs->kind == ast::TypeDeclKind::Class) {
            // §9.8 tier 2/tier 4: owned class-typed component.
            // `val` is a ptr to the class instance.
            // fix #3140 (family F2 of #3152): the component class's §9.8
            // surface and registry, from the symbol.
            if (!sema::serializedClassShape(fieldTs).empty()) {
                // §9.8 tier 4: if the class has a polymorphic registry, use
                // polymorphic dispatch (emitPolyClassSerialize) which reads the
                // dynamic type from the vtable and writes "$type" + concrete body.
                if (!sema::serializedPolyCasesOf(fieldTs).empty())
                    emitPolyClassSerialize(fieldTs, val, serPtr);
                else
                    emitClassSerialize(fieldTs, val, serPtr);
                return;
            }
        }
    }

    // fix #3140 follow-up (§9.8 / §2.8.3 rule 4): THIS USED TO BE A SILENT SKIP,
    // and that is the whole defect class. Reaching here means the dispatch above
    // recognised nothing, so the component contributes ZERO BYTES — while
    // `emitRecordSerialize`'s header count, computed from the component LIST
    // rather than from what the dispatch can write, still counts it. The reader
    // then runs off the end of the object: measured as an imported
    // `record InlineArr(A: [3]int, N: int)` writing 22 bytes against its local
    // twin's 50, with an imported-writer/local-reader exchange raising
    // "Truncated binary input reading int (§2.15)" and an imported-only
    // round-trip raising NOTHING AT ALL because both halves agreed on the same
    // wrong form.
    //
    // Sema now rejects the one shape known to reach here
    // (`sema::serializationComponentTypeIsEmittable` — an inline array whose
    // size did not cross), so this arm should be unreachable. It is a LOUD
    // backstop rather than a deleted branch precisely because the next carriage
    // gap must fail the build instead of rediscovering the 22-vs-50 hole. A
    // diagnostic here is idiomatic for this file: the `Shared<T>`/`Weak<T>`
    // "not yet implemented in codegen" guards above report the same way.
    (void)val;
    if (serializationDiagType_) {
        ctx_.diagnostics().report(
            Severity::Error, serializationDiagType_->declLoc,
            "`" + std::string(serializationDiagType_->name) +
            "`: a \xC2\xA7""9.8 serialized component has a type this compiler "
            "cannot write to the wire, so it would contribute no bytes while "
            "the object header still counts it (a desynchronised reader). This "
            "is a compiler limitation, not a program error \xE2\x80\x94 the "
            "known case is an inline array `[N]T` reached through an IMPORTED "
            "declaration, whose size does not cross the module boundary "
            "(issue #3183); mark the component `serialized skip`, or declare "
            "the type locally");
    }
}

// ---------------------------------------------------------------------------
// Emitter::emitEnumSerialize  §9.8
//
// For enum synthesis: writes the member NAME string (not the integer value).
//
// fix #3044 (§9.8 / §5.4 / §21.7, v0.242): §9.8 now reads "on write no failing
// case exists — every enum value is a declared value, §5.4", so this function's
// no-match arm is DEAD under the invariant. It is deliberately KEPT as a
// `bz_ser_throw` trap rather than deleted, for the reason §21.7 gives for its
// own statically-dead no-match edge: an implementation lowers a dead edge "as a
// defined trap … so that the grant's soundness never rests on the
// exhaustiveness computation alone". §9.8 does not mandate the trap, so deleting
// it would also conform — but of the two conforming lowerings this is the safe
// one, and the invariant is not yet airtight in this compiler: #3072 records
// that a zero-less enum FIELD or inline-array element still zero-fills, and
// §9.8 admits a non-owning inline array of enums as a record component, so the
// edge is currently REACHABLE. Deleting the raise would trade a loud
// `SerializationException` for silently writing the last-declared constant's
// name. Revisit once #3072 closes. The READ side's SerializationException (an
// encoded name matching no declared member) is untouched — that fault is about
// the *input*, not about a value of the enum type.
// ---------------------------------------------------------------------------
// fix #3140 (§9.8 / §2.8.3 rule 4, v0.252; family F4 of umbrella #3152): this
// used to bail out on `!ts->decl` and then iterate the `ast::TypeDecl`'s
// `enumConstants` for the wire name. `ModuleLoader` never assigns
// `Symbol::decl` for an imported type, so for an IMPORTED enum the function
// returned immediately and wrote NOTHING — no bytes, no diagnostic.
//
// MEASURED on 558c27c with a real two-module build: a LOCAL
// `record Holder(C: ColorE, N: int): ISerializable, IDeserializable` whose `C`
// component is an IMPORTED enum round-tripped `ColorE.Green` back as the wrong
// value (the program exited 2, its "C did not round-trip" code), while the
// identical declarations compiled as ONE module round-tripped and exited 0.
// Silent wrong DATA, which is the worst-shaped failure mode in the audit.
//
// Both facts now come off the symbol: the VALUE from `enumValue_` (which
// computeEnumValues fills for local and imported enums alike, #328), and the
// wire NAME from `sema::enumConstantWireNames` — a channel the artifact did not
// carry at all before this fix (`MetaMember::enumWireName`).
void Emitter::emitEnumSerialize(const TypeSymbol* ts,
                                llvm::Value* enumVal,
                                llvm::Value* serPtr) {
    if (!ts) return;
    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);

    // §10.3 (#740): compare at the enum's underlying width — truncating a
    // `: long` enum to i32 would mis-match (or collide) members whose values
    // differ only above bit 31.
    llvm::Type* uTy = enumLlvmType(ts);
    llvm::Value* ev = enumVal;
    if (ev->getType() != uTy)
        ev = builder_.CreateSExtOrTrunc(ev, uTy, "enum.ev");

    // Collect (wire name, underlying value) in DECLARATION order — §10.2's
    // shared-value rule wants the FIRST-declared name for a duplicated value,
    // and a first-match chain in declaration order delivers exactly that.
    // enumValue_ (computeEnumValues()) covers local and imported enums alike;
    // §10.3 (#740) keeps the full 64-bit value (a `: long` member exceeds i32).
    std::vector<std::pair<std::string, std::int64_t>> entries;
    for (const auto& [ec, wireName] : sema::enumConstantWireNames(*ts)) {
        auto it = enumValue_.find(ec);
        if (it != enumValue_.end())
            entries.emplace_back(std::string(wireName), it->second);
    }
    if (entries.empty()) return;  // no declared constants — nothing to write

    auto* writeFn =
        declareC("bz_binser_write_string", voidTy, {ptrTy(), ptrTy()});
    auto emitWrite = [&](const std::string& wireName) {
        llvm::Value* nameStr =
            lowerStringLiteral(std::string("\"") + wireName + "\"", false);
        builder_.CreateCall(writeFn, {serPtr, nameStr});
    };

    auto* endBB   = llvm::BasicBlock::Create(llvm_, "enum.ser.end",   currentFn_);
    auto* throwBB = llvm::BasicBlock::Create(llvm_, "enum.ser.throw", currentFn_);
    // Every declared constant gets its compare, INCLUDING the last: making the
    // last one an unconditional fallback would save one compare but would turn
    // the dead edge into "write the last-declared name" — silent wrong data
    // instead of the defined trap below.
    for (const auto& e : entries) {
        llvm::Value* cmpVal = llvm::ConstantInt::get(
            uTy, static_cast<std::uint64_t>(e.second), /*isSigned=*/true);
        llvm::Value* matches = builder_.CreateICmpEQ(ev, cmpVal, "enum.eq");
        auto* writeBB =
            llvm::BasicBlock::Create(llvm_, "enum.ser.write", currentFn_);
        auto* nextBB =
            llvm::BasicBlock::Create(llvm_, "enum.ser.next", currentFn_);
        builder_.CreateCondBr(matches, writeBB, nextBB);
        builder_.SetInsertPoint(writeBB);
        emitWrite(e.first);
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(nextBB);
    }

    // No constant matched — dead under §5.4's invariant, a defined trap anyway.
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
    // fix #3140 (§9.8, family F2 of umbrella #3152): the read twin of
    // emitRecordSerialize's conversion above — this opened with
    // `if (!ts || !ts->decl) return nullptr;` and then built its component list
    // out of `ast::TypeDecl::positionalFields` / each auto-property's
    // `ast::PropertyDecl::serialized`, neither of which an IMPORTED record has.
    // It returned null, which `lowerLocalDecl` degrades to a zero-initialized
    // slot (#2434) — the imported half of the measured `0 for an expected 34`.
    if (!ts) return nullptr;
    // fix #3140 follow-up: name this type in the backstop diagnostic.
    SerializationDiagScope diagScope(this, ts);
    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);

    // Collect serialized components. The list is 1:1 with `storage`, and so
    // with `recordStruct`'s elements: every downstream index (`slots[ci]`,
    // `st->getElementType(ci)`, the construction loop) assumes that alignment.
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

    // §9.8 / #2516: see emitRecordSerialize — for a record with a positional
    // parameter list the serialized components are its POSITIONAL fields, and
    // `posParam != nullptr` is that marker on both sides. A body member of a
    // combined-form record keeps a struct slot but stays off the wire, which is
    // what the AST walk did too (its component list came from
    // `positionalFields` alone); expressing it as `isSkip` is what keeps the
    // 1:1 alignment the loops below require.
    const bool positional = ts->hasPositionalFields;
    unsigned si = 0;
    for (const sema::Symbol* sym : storage) {
        const auto* fs = dynamic_cast<const sema::FieldSymbol*>(sym);
        const bool isComponent = !positional || (fs && fs->posParam != nullptr);
        const auto clause = sema::serializedComponentClause(sym);
        Component comp;
        comp.storageIdx = si;
        comp.declName   = std::string(sym->name);
        comp.isSkip     = !isComponent || clause.skip;
        comp.isOptional = isComponent && clause.optional;
        comp.wireName   = comp.isSkip ? "" : std::string(clause.wireName);
        comp.ty         = recordMemberTypeResolved(sym);
        components.push_back(std::move(comp));
        ++si;
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
    //
    // fix #2399 / §9.8 v0.235 + stdlib §2.15 v0.235 ("a duplicate field
    // name ... raises SerializationException unconditionally"; "missing
    // required fields, duplicates, and type mismatches remain unconditional
    // errors regardless of the [unknown-field] setting"): a seen-flag is now
    // allocated for EVERY non-skip component, not just required ones -- a
    // duplicate `serialized optional` field is equally a structural fault,
    // so the per-field dispatch loop below needs a flag to re-check even for
    // an optional component. `required` gates ONLY the missing-field check
    // further down (optional: missing is still fine).
    struct RecordSeenEntry {
        unsigned compIdx;
        std::string declName;
        llvm::AllocaInst* seenSlot;  // i8: 0=unseen, 1=seen
        bool required;               // false for `serialized optional` components
    };
    std::vector<RecordSeenEntry> recSeenFlags;
    for (unsigned i = 0; i < components.size(); ++i) {
        if (components[i].isSkip) continue;
        llvm::AllocaInst* sf = entryAlloca(i8());
        builder_.CreateStore(builder_.getInt8(0), sf);
        recSeenFlags.push_back(
            {i, components[i].declName, sf, !components[i].isOptional});
    }

    // §9.8 / #1635 leak-safety: the per-iteration field-name string returned by
    // bz_binder_read_field_name must be released even when a nested
    // emitDeserializeField throws (its emitThrowBuiltin calls emitUnwindDisposals
    // which sweeps ALL disposeStack_ entries, including this one). This mirrors
    // emitClassDeserialize's namePtrSlot (see below / the class path). Strategy:
    // keep one `namePtrSlot` alloca (null-initialized) holding the current
    // iteration's namePtr, registered as a stringRelease disposal so
    // emitUnwindDisposals releases it on the throw path. On the normal match/skip
    // path, after the explicit bz_string_release(namePtr), we null the slot to
    // prevent the disposal from double-releasing. Per §9.8/§28. Without this the
    // field name read just before a throwing value read (e.g. JSON type mismatch)
    // leaks — the #1635 defect.
    //
    // fix #669: null-init MUST be in the entry block (emitRecordDeserialize can
    // be called from inside a conditional branch). See emitClassDeserialize's
    // namePtrSlot for the full rationale.
    llvm::AllocaInst* namePtrSlot = entryAlloca(ptrTy());
    {
        llvm::BasicBlock& eb = currentFn_->getEntryBlock();
        llvm::IRBuilder<> eb_b(&eb, std::next(namePtrSlot->getIterator()));
        eb_b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), namePtrSlot);
    }
    if (!disposeStack_.empty()) {
        Disposal nd{namePtrSlot, nullptr};
        nd.stringRelease = true;
        disposeStack_.back().push_back(nd);
    }

    // fix #2398 / §9.8 / §28.3/§28.9: a dedicated scope guarding already-
    // populated component slots against a LATER component's read throwing —
    // mirrors #2378's emitClassDeserialize fix (Bug B) for the record case.
    // A record's serialized components are record-admissible only (§9.8:
    // "primitive, string, an enum, another serialization-eligible record,
    // a value-like serialization-eligible choice, T? of a record-admissible
    // T, or a non-owning inline array" — no class-, interface-, Shared<T>-,
    // Weak<T>-, or heap-collection-typed component is ever admitted), so
    // there is no class half to mirror here — but §4.6's string is a
    // runtime-managed +1 reference like any other, and a nested record
    // component may itself own one. Pushed before any component is read;
    // each string-typed slot is registered as a plain stringRelease
    // disposal, and each NESTED-RECORD-typed slot whose own storage owns a
    // string (recordHasStringFields, the #1747 mechanism) is registered to
    // release ITS direct string members via the existing
    // emitRecordStringFieldReleases helper — reusing #1747's proven,
    // one-level-deep release rather than inventing new machinery. (That
    // reuse is intentionally narrow, matching #1747's own scope: it does
    // NOT recurse into a nested record's own nested-record/choice
    // components, and it excludes a NULLABLE record component, whose slot
    // is the wrapped {i8, T} struct, not the bare record layout
    // emitRecordStringFieldReleases expects — isRecordWithStringFieldsTypeRef
    // already guards both. A record component that is itself a value-like
    // CHOICE with a string payload, or a record nested TWO OR MORE levels
    // deep, is a documented residual gap: no equivalent release helper
    // exists for choices yet, and the reused helper here is one level
    // only — issue #2405 filed.)
    // Popped WITHOUT disposal once every required component is confirmed
    // present (below, just before construction begins) — by then each
    // slot's value is about to be consumed into the constructed record.
    //
    // Without this: a component slot populated by an EARLIER successful
    // read (e.g. `first: string` read before `second: int`) had no
    // disposal registered anywhere. A LATER component's read that throws —
    // a truncated wire value, or a nested record/choice Deserialize that
    // throws from deep inside its own recursive call — unwinds straight
    // past this function's own explicit skip/match dispatch (which only
    // releases namePtr, never a component slot), orphaning the already-read
    // value (issue #2398, the same shape as #2378's own follow-up leak;
    // empirically confirmed: 63 bytes / 2 allocations for the issue's exact
    // record repro, matching #2378's class-side signature exactly).
    disposeStack_.emplace_back();
    for (unsigned i = 0; i < components.size(); ++i) {
        if (components[i].isSkip || !slots[i] || !components[i].ty) continue;
        const auto* cty = components[i].ty;
        if (cty->kind == sema::TypeRef::Kind::Primitive &&
            cty->primitive == sema::PrimitiveType::String) {
            Disposal sd{slots[i], nullptr};
            sd.stringRelease = true;
            disposeStack_.back().push_back(sd);
        } else if (isRecordWithStringFieldsTypeRef(cty)) {
            Disposal rd{slots[i], cty->declaration};
            rd.recordStringFields = true;
            // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): carry `cty` itself
            // (genericArgs included, e.g. a `Box<string>` component) through
            // to the release — see Disposal::clsRef (LlvmBackend_impl.hpp).
            rd.clsRef = cty;
            disposeStack_.back().push_back(rd);
        } else if (!cty->nullable && cty->kind == sema::TypeRef::Kind::Named &&
                   cty->declaration &&
                   cty->declaration->kind == ast::TypeDeclKind::Choice &&
                   choiceHasStringFields(cty->declaration)) {
            // fix #2425 / §9.8 / §28.3/§28.9 / §35.4: a record component may
            // itself be a value-like CHOICE (record-admissible per §9.8)
            // that owns a string somewhere reachable from its active case —
            // the residual gap #2405 documented (no release walker existed
            // for choices at all). Released via emitChoiceStringFieldReleases,
            // a separate tag-switched walker (see the Disposal.choiceStringFields
            // comment in LlvmBackend_impl.hpp).
            Disposal cd{slots[i], cty->declaration};
            cd.choiceStringFields = true;
            disposeStack_.back().push_back(cd);
        } else if (!cty->nullable &&
                   cty->kind == sema::TypeRef::Kind::InlineArray &&
                   cty->arrayElement &&
                   cty->arraySizeValue >= 0 &&
                   inlineArrayElementCarriesString(cty->arrayElement)) {
            // fix #2326 / §9.8 v0.228: a non-nullable INLINE-ARRAY record
            // component whose element type (recursively) carries a string
            // — `[N]string`, `[N]RecWithAStringField`, etc. Registered for
            // the same reason as the recordStringFields arm above: a LATER
            // sibling's read could throw, and without a registered release
            // the already-read inline-array slot's element strings would
            // orphan. Released via emitInlineArrayStringReleases, the
            // array-side mirror of choiceStringFields (its own walker,
            // because the array's element-dispatch has no analog of the
            // record/choice field-index machinery). The slot's LLVM type
            // is `[N x elem]`, the disposer's element TypeRef drives
            // the per-element dispatch (string → release; record → recurse;
            // choice → choice walker; nested array → recurse).
            Disposal ad{slots[i], nullptr};
            ad.inlineArrayStringFields = true;
            ad.inlineArrayElemType = cty;
            disposeStack_.back().push_back(ad);
        }
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
    // §9.8 / #1635: record the live namePtr so emitUnwindDisposals can release
    // it if a nested emitDeserializeField throws before the normal match/skip
    // release below. (null-safe: a null namePtr stored here is a no-op release.)
    builder_.CreateStore(namePtr, namePtrSlot);

    // fix #2404 / §9.8 / §28.9: bz_binder_read_field_name can still return a
    // NULL bz_string* WITHOUT a pending exception on the OOM allocation-
    // failure paths inside bz_binder_read_bz_string (malloc/calloc returning
    // null while building the decoded string/header) -- the ordinary
    // truncated-input path is already caught by the emitPendingCheck() just
    // above, since fix #2403 made bz_binder_read_bz_string raise
    // SerializationException before ever returning null on truncation (both
    // the binary length/data-truncation case and, transitively, the JSON
    // unterminated-object case, which bz_jder_count_object_fields already
    // rejects before this loop's first iteration). Without this check, a
    // null namePtr here is dereferenced unguarded a few lines down
    // (CreateStructGEP/CreateLoad against namePtr for the wire-name
    // comparison) -- a null-pointer read. Mirrors emitClassDeserialize's
    // identical defence (its errBB, ~line 3144) for the exact same
    // OOM-residual gap; unlike the class path, no manual per-slot dispose
    // loop is needed here -- the #2398 disposeStack_ scope pushed above
    // (before this loop started) already registers every already-filled
    // component slot (string / one-level nested-record) for automatic
    // release, and emitThrowBuiltin's emitUnwindDisposals() sweeps it on
    // this throw exactly as it does for every other throw in this function
    // (including namePtrSlot itself, just stored above).
    {
        llvm::Value* isNull = builder_.CreateICmpEQ(
            namePtr, llvm::ConstantPointerNull::get(ptrTy()), "deser.fname.isnull");
        auto* nullBB = llvm::BasicBlock::Create(llvm_, "deser.fname.null", currentFn_);
        auto* liveBB = llvm::BasicBlock::Create(llvm_, "deser.fname.live", currentFn_);
        builder_.CreateCondBr(isNull, nullBB, liveBB);
        builder_.SetInsertPoint(nullBB);
        emitThrowBuiltin("SerializationException");
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateUnreachable();
        builder_.SetInsertPoint(liveBB);
    }

    // Dispatch: compare namePtr against each component's wire name.
    // Use bz_str_compare_cp (the runtime's ordinal, code-point-wise string
    // comparison) to test equality: bz_str_compare_cp(a, b, ignoreCase) → i32.
    // fix #4487 (stdlib §9.6 + spec §9.8, v0.306): the wire-name dispatch is an
    // EQUALITY test (`cmp == 0`) and `ignoreCase` is always 0 here — §9.8:
    // "Names are compared ORDINALLY". It used to extract (data, len) from each
    // side and call `bz_string_compare`, which walked `min(len)` BYTES over a
    // CODE-POINT length (the #535 defect class), so two distinct non-ASCII
    // names sharing their first `len` STORAGE bytes matched each other.
    // `bz_binder_read_string` decodes the wire's UTF-8 into a width-aware
    // bz_string, so both operands here really can be width 2/4. Measured on
    // HEAD (4d9d186a): an enum with `serialized "日本"` and `serialized "日x"`
    // round-tripped the SECOND constant back as the FIRST, because both are
    // width 2 and share the storage bytes `E5 65`. `bz_str_compare_cp` takes
    // the string OBJECTS, so each is decoded at its own width. Nothing changes
    // for an ASCII wire name, which is every name this suite writes today.
    auto* cmpFn = declareC("bz_str_compare_cp", i32(),
                           {ptrTy(), ptrTy(), i32()});

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

        llvm::Value* cmpRes = builder_.CreateCall(cmpFn,
            {namePtr, litStr, builder_.getInt32(0)}, "deser.cmp");
        llvm::Value* isMatch = builder_.CreateICmpEQ(
            cmpRes, builder_.getInt32(0), "deser.match");

        auto* readBB = llvm::BasicBlock::Create(llvm_, "deser.rd", currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "deser.nd", currentFn_);
        builder_.CreateCondBr(isMatch, readBB, nextBB);

        builder_.SetInsertPoint(readBB);

        // fix #2399 / §9.8 v0.235 + stdlib §2.15 v0.235 (quoted above the
        // recSeenFlags allocation): a component matched a SECOND time by the
        // wire (its seen-flag already 1) is a duplicate field name -- raised
        // BEFORE reading anything for this occurrence, so the value already
        // sitting in slots[ci] from the first occurrence is left untouched
        // for the #2398 disposal scope (pushed above) to release on the
        // emitThrowBuiltin unwind below -- mirrors emitClassDeserialize's
        // identical fix.
        {
            llvm::AllocaInst* mySeenSlot = nullptr;
            for (const auto& rse : recSeenFlags) {
                if (rse.compIdx == ci) { mySeenSlot = rse.seenSlot; break; }
            }
            if (mySeenSlot) {
                llvm::Value* alreadySeen = builder_.CreateLoad(
                    i8(), mySeenSlot, "rdeser.dup.seen");
                llvm::Value* isDup = builder_.CreateICmpNE(
                    alreadySeen, builder_.getInt8(0), "rdeser.dup.isdup");
                auto* dupBB = llvm::BasicBlock::Create(
                    llvm_, "rdeser.dup", currentFn_);
                auto* freshBB = llvm::BasicBlock::Create(
                    llvm_, "rdeser.fresh", currentFn_);
                builder_.CreateCondBr(isDup, dupBB, freshBB);

                builder_.SetInsertPoint(dupBB);
                emitThrowBuiltin("SerializationException");
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateUnreachable();

                builder_.SetInsertPoint(freshBB);
            }
        }
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
        // §9.8 / #1635: null the slot so the stringRelease disposal doesn't
        // double-release on the normal path (or at scope-end disposal).
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), namePtrSlot);
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
    // §9.8 / #1635: null the slot so the stringRelease disposal doesn't
    // double-release on the normal path (or at scope-end disposal).
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), namePtrSlot);
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
    // fix #2398: this missing-required-component throw is now ALSO covered
    // by the component-disposal scope pushed above (still open at this
    // point — it is popped only below, once construction is about to
    // begin) — an already-read string/nested-record slot is disposed on
    // this path too, not just a sibling's-read-throws path.
    // fix #2399: recSeenFlags now also holds entries for OPTIONAL components
    // (so the dispatch loop's duplicate check above can see them); skip
    // those here -- missing stays fine for an optional field.
    for (const auto& rse : recSeenFlags) {
        if (!rse.required) continue;
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

    // fix #2398 / §9.8 / §28.3/§28.9: pop the component-disposal scope
    // pushed above (guarding already-populated string/nested-record slots
    // against a later throw during the field loop / the missing-required-
    // component check just above). Every required component is now
    // confirmed present and construction is about to begin, consuming each
    // slot's value into the constructed record — nothing in this scope
    // should be disposed on this (normal) path, so pop it WITHOUT invoking
    // its disposals (mirrors #2378's emitClassDeserialize pop).
    disposeStack_.pop_back();

    // fix #5149 (§9.8 step 2, v0.309, #4775 Q2): the record constructor-
    // selection rule has THREE branches, not two:
    //   1. Exactly one EXPLICIT (non-static) constructor's parameter list
    //      binds exactly the record's components — call IT (its body runs
    //      with the deserialized slots as its args, its `requires`/`ensures`
    //      clauses fire per §22.16 — §9.8 step 2 "more than one matches" is
    //      read over the EXPLICIT constructors and the matching one wins).
    //   2. Zero explicit ctors match AND the record declares positional
    //      fields → the synthesized Deserialize calls the AUTO-GENERATED
    //      PRIMARY of a record declaring positional fields (§9.3). The
    //      primary assigns the positional fields, in declaration order, from
    //      the deserialized slots — equivalent to a raw field-insert for the
    //      positional components (body auto-property components of a
    //      combined-form record stay at default(T) — §9.3 "the auto-
    //      generated primary assigns the positional storage only"). The
    //      primary carries no clause (§9.3 "the auto-generated primary
    //      constructor can carry none"), so no contract evaluation runs.
    //   3. Zero explicit ctors match AND the record has no positional
    //      fields → impossible per sema (TypeResolution_Checks.cpp's
    //      checkSerializationEligibility reports "no explicit constructor
    //      has parameter names matching" before codegen runs); the loop
    //      below is a defensive fallback (storage-level construction from
    //      the read components — the same path the spec prescribes for "a
    //      record that declares no explicit constructor").
    // The pre-fix code labelled this branch as the no-explicit-ctor case
    // only and was silent on the synthesized-primary fallback; #5149 makes
    // branch (2) explicit.
    std::unordered_map<std::string, unsigned> compByNameLower;
    for (unsigned ci = 0; ci < components.size(); ++ci)
        compByNameLower[toLowerAscii(components[ci].declName)] = ci;

    sema::ConstructorSymbol* matchedCtor = nullptr;
    for (Symbol* m : ts->members) {
        auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
        if (!cs || cs->isStatic) continue;
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) continue;
        if (cd->params.size() != components.size()) continue;
        bool allMatch = true;
        for (const auto& p : cd->params) {
            if (compByNameLower.find(toLowerAscii(p.name)) ==
                    compByNameLower.end()) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) { matchedCtor = cs; break; }
    }

    llvm::Value* result = nullptr;
    if (matchedCtor) {
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(matchedCtor->decl);
        auto ctorIt = ctorFns_.find(matchedCtor);
        if (cd && ctorIt != ctorFns_.end()) {
            // Run storage-member default initializers first, mirroring
            // ordinary record construction (buildRecordValue's explicit-
            // constructor path, LlvmBackend_Exprs2.cpp): a component the
            // constructor body does not itself assign keeps its own
            // declared default, exactly as `new`/`with` construction would
            // leave it.
            for (const sema::Symbol* sm : storage) {
                const ast::Expr* init = nullptr;
                if (auto* fd = dynamic_cast<const ast::FieldDecl*>(sm->decl))
                    init = fd->initializer.get();
                else if (auto* pd =
                             dynamic_cast<const ast::PropertyDecl*>(sm->decl))
                    init = pd->autoInitializer.get();
                if (!init) continue;
                auto fit = fieldIndex_.find(sm);
                if (fit == fieldIndex_.end()) continue;
                llvm::Value* v = lowerExpr(*init);
                if (!v) continue;
                const sema::TypeRef* fty = recordMemberType(sm);
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
                    v, builder_.CreateStructGEP(st, resultSlot, fit->second));
            }

            // Build the argument list IN CONSTRUCTOR PARAMETER ORDER, each
            // pulled from its component's deserialized slot (or default(T)
            // for a `serialized skip` component — §9.8).
            std::vector<llvm::Value*> args;
            args.push_back(resultSlot);  // `this` (a pointer to the record's
                                          // own struct slot, §9.4)
            auto* fnTy = ctorIt->second->getFunctionType();
            bool ok = true;
            for (std::size_t pi = 0; pi < cd->params.size(); ++pi) {
                auto cit = compByNameLower.find(
                    toLowerAscii(cd->params[pi].name));
                if (cit == compByNameLower.end()) { ok = false; break; }
                unsigned ci = cit->second;
                if (!components[ci].ty) { ok = false; break; }
                llvm::Type* lt = lowerTypeRef(components[ci].ty);
                if (!lt) { ok = false; break; }
                llvm::Value* fv;
                if (components[ci].isSkip || !slots[ci])
                    fv = llvm::Constant::getNullValue(lt);
                else
                    fv = builder_.CreateLoad(lt, slots[ci], "rdeser.arg");
                if (pi + 1 < fnTy->getNumParams())
                    fv = coerceNumericToType(fv, fnTy->getParamType(pi + 1),
                                              false);
                args.push_back(fv);
            }
            if (ok && args.size() == ctorIt->second->arg_size()) {
                builder_.CreateCall(ctorIt->second, args);
                // §28: a record constructor may throw (e.g. a validating
                // constructor, §9.2's `Width > 0 and Height > 0` example).
                emitPendingCheck();
                result = builder_.CreateLoad(st, resultSlot, "deser.rec");
            }
        }
    }

    if (!result) {
        // fix #5149 (§9.8 step 2, v0.309, #4775 Q2): no explicit ctor matched.
        // Build the result struct directly from the deserialized slots. The
        // exact behavior depends on whether the record declares positional
        // fields (auto-generated primary exists) or not:
        //
        //   * hasPositionalFields → the synthesized Deserialize runs the
        //     AUTO-GENERATED PRIMARY, which per §9.3 "assigns the positional
        //     storage only". `components` already excludes body auto-property
        //     components of a combined-form record (`isComponent` is true
        //     only for symbols whose `posParam != nullptr`, see the
        //     components-collection loop above), so the storage-level
        //     insert loop below naturally assigns positional fields only
        //     and leaves body auto-properties at the zero-init default
        //     populated at the entry-alloca above.
        //
        //   * !hasPositionalFields → impossible per sema
        //     (TypeResolution_Checks.cpp's checkSerializationEligibility
        //     rejects this with "no explicit constructor has parameter
        //     names matching the serialized component set ... synthesized
        //     primary is the fallback candidate when the record declares
        //     positional fields" before codegen runs); the loop is the
        //     defensive storage-level fallback the spec prescribes for
        //     records with no explicit constructor (§9.8 v0.182: "A record
        //     that declares no explicit constructor is constructed
        //     directly from the read components at the storage level").
        result = builder_.CreateLoad(st, resultSlot, "deser.rec");
        for (unsigned ci = 0; ci < components.size(); ++ci) {
            if (components[ci].isSkip) continue;
            if (!slots[ci]) continue;
            if (ci >= st->getNumElements()) continue;
            llvm::Value* fv = builder_.CreateLoad(st->getElementType(ci), slots[ci]);
            result = builder_.CreateInsertValue(result, fv, {ci});
        }
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
            // fix #2378 / §9.8 / §2.15: bz_binder_read_bool can raise (JSON:
            // "Expected JSON bool") — mirror lowerDeserializerReadMethod's
            // ReadBool, which already pending-checks every read. Without this,
            // a pending exception set here is left unchecked and later
            // SUPERSEDED (without disposal) by a subsequent, unrelated throw
            // elsewhere in this recursive field loop — leaking the exception
            // object + its message (the #2378 pattern).
            emitPendingCheck();
            return builder_.CreateTrunc(b8, i1(), "deser.bool1");
        }
        case sema::PrimitiveType::Char: {
            auto* fn = declareC("bz_binder_read_char", i32(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.char");
            emitPendingCheck();  // fix #2378 — see the Bool case above.
            return v;
        }
        case sema::PrimitiveType::Byte: {
            auto* fn = declareC("bz_binder_read_uint", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.u64");
            // fix #2378 / §9.8 / §2.15: bz_binder_read_uint raises
            // SerializationException on truncated binary input
            // (bz_ser_throw_at_binary) — see the Bool case above for why an
            // unchecked pending exception here leaks.
            emitPendingCheck();
            return builder_.CreateTrunc(v, i8(), "deser.byte");
        }
        case sema::PrimitiveType::SByte: {
            auto* fn = declareC("bz_binder_read_int", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.i64");
            emitPendingCheck();  // fix #2378 — see the Byte case above.
            return builder_.CreateTrunc(v, i8(), "deser.sbyte");
        }
        case sema::PrimitiveType::Short: {
            auto* fn = declareC("bz_binder_read_int", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.i64");
            emitPendingCheck();  // fix #2378 — see the Byte case above.
            return builder_.CreateTrunc(v, i16(), "deser.short");
        }
        case sema::PrimitiveType::UShort: {
            auto* fn = declareC("bz_binder_read_uint", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.u64");
            emitPendingCheck();  // fix #2378 — see the Byte case above.
            return builder_.CreateTrunc(v, i16(), "deser.ushort");
        }
        case sema::PrimitiveType::Int: {
            auto* fn = declareC("bz_binder_read_int", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.i64");
            // fix #2378 / §9.8 / §2.15 (the reported repro): bz_binder_read_int
            // raises SerializationException on truncated binary input — see the
            // Bool case above for why an unchecked pending exception here leaks
            // the exception object bz_ser_throw allocated.
            emitPendingCheck();
            return builder_.CreateTrunc(v, i32(), "deser.int");
        }
        case sema::PrimitiveType::UInt: {
            auto* fn = declareC("bz_binder_read_uint", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.u64");
            emitPendingCheck();  // fix #2378 — see the Byte case above.
            return builder_.CreateTrunc(v, i32(), "deser.uint");
        }
        case sema::PrimitiveType::Long: {
            auto* fn = declareC("bz_binder_read_int", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.long");
            emitPendingCheck();  // fix #2378 — see the Byte case above.
            return v;
        }
        case sema::PrimitiveType::ULong: {
            auto* fn = declareC("bz_binder_read_uint", i64(), {ptrTy()});
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.ulong");
            emitPendingCheck();  // fix #2378 — see the Byte case above.
            return v;
        }
        case sema::PrimitiveType::Float: {
            auto* fn = declareC("bz_binder_read_float",
                                llvm::Type::getDoubleTy(llvm_), {ptrTy()});
            llvm::Value* d = builder_.CreateCall(fn, {derPtr}, "deser.dbl");
            emitPendingCheck();  // fix #2378 — see the Byte case above.
            return builder_.CreateFPTrunc(d, llvm::Type::getFloatTy(llvm_), "deser.f");
        }
        case sema::PrimitiveType::Double: {
            auto* fn = declareC("bz_binder_read_float",
                                llvm::Type::getDoubleTy(llvm_), {ptrTy()});
            llvm::Value* d = builder_.CreateCall(fn, {derPtr}, "deser.dbl");
            emitPendingCheck();  // fix #2378 — see the Byte case above.
            return d;
        }
        case sema::PrimitiveType::Decimal: {
            // §2.14 / #665: ReadDecimal — bz_binder_read_decimal returns a
            // void* (ptr to alloca'd 16-byte struct), load it back to a value.
            auto* fn = declareC("bz_binder_read_decimal", ptrTy(), {ptrTy()});
            llvm::Value* decPtr = builder_.CreateCall(fn, {derPtr}, "deser.decptr");
            // fix #2378 / §9.8 / §2.15: the JSON path (bz_jder_parse_decimal)
            // can raise SerializationException — see the Bool case above.
            emitPendingCheck();
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
            llvm::Value* v = builder_.CreateCall(fn, {derPtr}, "deser.str");
            // fix #2378 / §9.8 / §2.15: bz_binder_read_string's JSON path can
            // raise SerializationException — see the Bool case above. (The
            // binary path's own truncated-payload leak is fixed separately in
            // emitClassDeserialize/emitRecordDeserialize's per-slot disposal
            // registration — the successfully-read string itself, not an
            // exception object, is what leaks there; see the #2378 commit.)
            emitPendingCheck();
            return v;
        }
        default:
            return nullptr;
        }
    }

    // §9.8 v0.228 (issue #1065): a non-owning inline array [N]T — the mirror
    // of the emitSerializeField branch above. N is compile-time-constant
    // (arraySizeValue, folded by sema per §15.1), so the reconstruction
    // materializes element-by-element into a stack alloca and loads the
    // aggregate back, exactly the pattern LlvmBackend_Assign.cpp's inline-
    // array-typed call-argument collection-literal path already uses (#1382)
    // — inline arrays are value types, so no disposal registration is
    // needed. BeginArray()/EndArray() still bracket the read (consuming the
    // wire's own count prefix / '['..']' delimiters per stdlib §2.15) even
    // though the loop itself is driven by the DECLARED size N, not the wire
    // count: a truncated or malformed wire array is a structural fault the
    // per-element reads surface as SerializationException (e.g. an
    // out-of-data primitive read), consistent with §9.8's "a type mismatch
    // ... raises SerializationException unconditionally".
    if (fieldTy->kind == K::InlineArray && !fieldTy->arrayOwned &&
        fieldTy->arrayElement && fieldTy->arraySizeValue >= 0) {
        llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);
        llvm::Type* elemLlvmTy = lowerTypeRef(fieldTy->arrayElement);
        auto n = static_cast<unsigned>(fieldTy->arraySizeValue);
        if (elemLlvmTy) {
            auto* arrLlvmTy = llvm::ArrayType::get(elemLlvmTy, n);
            auto* beginFn = declareC("bz_binder_begin_array", i32(), {ptrTy()});
            builder_.CreateCall(beginFn, {derPtr}, "deser.arr.n");
            llvm::AllocaInst* slot = entryAlloca(arrLlvmTy);
            builder_.CreateStore(llvm::ConstantAggregateZero::get(arrLlvmTy), slot);
            for (unsigned i = 0; i < n; ++i) {
                llvm::Value* elemVal =
                    emitDeserializeField(fieldTy->arrayElement, derPtr);
                if (!elemVal) continue;
                llvm::Value* gep = builder_.CreateGEP(
                    arrLlvmTy, slot,
                    {builder_.getInt32(0), builder_.getInt32(i)});
                builder_.CreateStore(elemVal, gep);
            }
            auto* endFn = declareC("bz_binder_end_array", voidTy, {ptrTy()});
            builder_.CreateCall(endFn, {derPtr});
            return builder_.CreateLoad(arrLlvmTy, slot, "deser.inlinearr");
        }
        return nullptr;
    }

    if (fieldTy->kind == K::Named && fieldTy->declaration) {
        const TypeSymbol* fieldTs = fieldTy->declaration;

        if (fieldTs->kind == ast::TypeDeclKind::Enum)
            return emitEnumDeserialize(fieldTs, derPtr);

        if (fieldTs->kind == ast::TypeDeclKind::Record)
            return emitRecordDeserialize(fieldTs, derPtr);

        // §9.8 choice serialization (v0.200, #2046): a nested value-like choice
        // component reconstructs via the `$case` object form.
        if (fieldTs->kind == ast::TypeDeclKind::Choice)
            return emitChoiceDeserialize(fieldTs, derPtr);

        if (fieldTs->kind == ast::TypeDeclKind::Class) {
            // §9.8 tier 2/tier 4: deserialize a class-typed component.
            // Returns an owned ptr.
            // fix #3140 (family F2 of #3152): the component class's §9.8
            // surface and registry, from the symbol.
            if (!sema::serializedClassShape(fieldTs).empty()) {
                // §9.8 tier 4: polymorphic base — dispatch via "$type" discriminator.
                if (!sema::serializedPolyCasesOf(fieldTs).empty())
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
//
// fix #3140 (§9.8 / §2.8.3 rule 4, v0.252; family F4 of umbrella #3152): the
// read half of the same defect — for an IMPORTED enum this returned a constant
// `i32 0` without consuming the wire string at all. See emitEnumSerialize's
// header for the measurement.
llvm::Value* Emitter::emitEnumDeserialize(const TypeSymbol* ts,
                                          llvm::Value* derPtr) {
    if (!ts) return builder_.getInt32(0);
    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);

    // Read the string.
    auto* readFn = declareC("bz_binder_read_string", ptrTy(), {ptrTy()});
    llvm::Value* namePtr = builder_.CreateCall(readFn, {derPtr}, "deser.ename");

    // §10.3 (#740): the result enum value at the enum's underlying width, so a
    // deserialized `: long` member keeps all 64 bits.
    llvm::Type* uTy = enumLlvmType(ts);
    llvm::AllocaInst* resultSlot = entryAlloca(uTy);
    builder_.CreateStore(llvm::ConstantInt::get(uTy, 0), resultSlot);

    auto* endBB   = llvm::BasicBlock::Create(llvm_, "enum.deser.end",   currentFn_);
    auto* throwBB = llvm::BasicBlock::Create(llvm_, "enum.deser.throw", currentFn_);

    // fix #4487 (stdlib §9.6 + spec §9.8, v0.306): the wire-name dispatch is an
    // EQUALITY test (`cmp == 0`) and `ignoreCase` is always 0 here — §9.8:
    // "Names are compared ORDINALLY". It used to extract (data, len) from each
    // side and call `bz_string_compare`, which walked `min(len)` BYTES over a
    // CODE-POINT length (the #535 defect class), so two distinct non-ASCII
    // names sharing their first `len` STORAGE bytes matched each other.
    // `bz_binder_read_string` decodes the wire's UTF-8 into a width-aware
    // bz_string, so both operands here really can be width 2/4. Measured on
    // HEAD (4d9d186a): an enum with `serialized "日本"` and `serialized "日x"`
    // round-tripped the SECOND constant back as the FIRST, because both are
    // width 2 and share the storage bytes `E5 65`. `bz_str_compare_cp` takes
    // the string OBJECTS, so each is decoded at its own width. Nothing changes
    // for an ASCII wire name, which is every name this suite writes today.
    auto* cmpFn = declareC("bz_str_compare_cp", i32(),
                           {ptrTy(), ptrTy(), i32()});

    for (const auto& [ec, wire] : sema::enumConstantWireNames(*ts)) {
        auto it = enumValue_.find(ec);
        if (it == enumValue_.end()) continue;
        const std::int64_t ordinal = it->second;

        std::string wireName = std::string(wire);
        llvm::Value* litStr  = lowerStringLiteral(
            std::string("\"") + wireName + "\"", false);

        llvm::Value* cmpRes  = builder_.CreateCall(cmpFn,
            {namePtr, litStr, builder_.getInt32(0)}, "enum.cmp");
        llvm::Value* isMatch = builder_.CreateICmpEQ(
            cmpRes, builder_.getInt32(0), "enum.match");

        auto* matchBB = llvm::BasicBlock::Create(llvm_, "enum.deser.match", currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "enum.deser.next",  currentFn_);
        builder_.CreateCondBr(isMatch, matchBB, nextBB);

        builder_.SetInsertPoint(matchBB);
        builder_.CreateStore(
            llvm::ConstantInt::get(uTy, static_cast<std::uint64_t>(ordinal),
                                   /*isSigned=*/true),
            resultSlot);
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
    return builder_.CreateLoad(uTy, resultSlot, "enum.deser.v");
}

// ---------------------------------------------------------------------------
// Emitter::emitChoiceSerialize  §9.8 choice serialization (v0.200, #2046)
//
// The `$case`-discriminated object form for a value-like choice.  The choice
// struct is `{ i32 tag, case0_payload, case1_payload, ... }` (choiceStruct);
// `choiceVal` is a loaded struct value.  Switch on the active-case tag; each arm
// writes an object whose distinguished leading field is `"$case"` (the case's
// declared name, or its `serialized "<name>"` rename), followed by that case's
// payload fields by name in declaration order.  A payload-free case writes the
// object with only the discriminator.  Mirrors emitChoiceEquality's switch shape
// so the per-case payload writes run only under the matching tag.
// ---------------------------------------------------------------------------
void Emitter::emitChoiceSerialize(const TypeSymbol* ts,
                                  llvm::Value* choiceVal,
                                  llvm::Value* serPtr) {
    // fix #3185 (§9.8 / §35, v0.252; family F2 of umbrella #3152): this opened
    // with `if (!ts || !ts->decl) return;`. `ModuleLoader` never assigns
    // `Symbol::decl` for an imported type, so for an IMPORTED choice this
    // function returned immediately and wrote NOTHING — no bytes, no
    // diagnostic. MEASURED on a two-module build with the classification gate
    // forced open and nothing else changed:
    //
    //   direct `Status.Deserialize(d)` round-trip   IMPORTED exit 0, LOCAL 7
    //     (plus the #2434 "degraded to a zero-initialized value" note)
    //   local `record Holder(S: Status, N: int)`    IMPORTED exit 0, LOCAL 8
    //     — NO DIAGNOSTIC AT ANY SEVERITY
    //   case `serialized "hh"` rename oracle        SerializationException
    //     — the case rename does not cross either
    //
    // The body reads `cc->caseDecl` (never `ts->decl`), and F9 (#3144,
    // `feec091`) reconstructed `caseDecl` — name, payload field names, the
    // §28.1 `owned` marker, each payload's resolved type (published via
    // `setResolvedType`), AND the §9.8 `serialized` clause on the case AND
    // each payload field — for imported choices. So the `ts->decl` guard was
    // the whole defect: the predicate fed by an empty source. Dropped it, and
    // `choiceCasesOf(ts)` / `choiceStruct(ts)` / `cc->caseDecl->payload` all
    // walk the imported representation correctly. `ctx.isImported` (not a
    // `t->decl` read) is the symbol-side question the eligibility gate uses.
    if (!ts) return;
    // fix #3140 follow-up: name this type in the backstop diagnostic.
    SerializationDiagScope diagScope(this, ts);
    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);
    auto cases = choiceCasesOf(ts);
    if (cases.empty()) return;

    // Active-case tag (field 0 of the choice struct).
    llvm::Value* tag = builder_.CreateExtractValue(choiceVal, {0u}, "chser.tag");

    auto* endBB = llvm::BasicBlock::Create(llvm_, "chser.end", currentFn_);
    // Switch default → end (writes nothing; unreachable for a valid value, whose
    // tag is always an in-range case index).
    auto* sw = builder_.CreateSwitch(tag, endBB,
                                     static_cast<unsigned>(cases.size()));

    auto* beginFn     = declareC("bz_binser_begin_object", voidTy, {ptrTy(), i32()});
    auto* endFn       = declareC("bz_binser_end_object", voidTy, {ptrTy()});
    auto* writeNameFn = declareC("bz_binser_write_field_name", voidTy,
                                 {ptrTy(), ptrTy()});
    auto* writeStrFn  = declareC("bz_binser_write_string", voidTy, {ptrTy(), ptrTy()});

    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        const sema::ChoiceCaseSymbol* cc = cases[ci];
        auto* armBB = llvm::BasicBlock::Create(llvm_, "chser.case", currentFn_);
        sw->addCase(builder_.getInt32(static_cast<int>(ci)), armBB);
        builder_.SetInsertPoint(armBB);

        // §9.8: field count = 1 ($case) + the active case's non-skip payloads.
        int32_t nonSkip = 0;
        if (cc && cc->caseDecl)
            for (const auto& pf : cc->caseDecl->payload)
                if (!positionalFieldIsSkip(pf)) ++nonSkip;

        builder_.CreateCall(beginFn, {serPtr, builder_.getInt32(1 + nonSkip)});
        // §2.15 / #687: begin_object can set bz_exc (MaxDepth guard).
        emitPendingCheck();

        // Leading discriminator: "$case" → the case's wire name.
        llvm::Value* caseKey = lowerStringLiteral("\"$case\"", false);
        builder_.CreateCall(writeNameFn, {serPtr, caseKey});
        std::string caseWire(choiceCaseWireName(cc));
        llvm::Value* caseVal = lowerStringLiteral("\"" + caseWire + "\"", false);
        builder_.CreateCall(writeStrFn, {serPtr, caseVal});

        // Active case's payload fields (declaration order, skip excluded).
        if (cc && cc->caseDecl) {
            for (std::size_t pi = 0; pi < cc->caseDecl->payload.size(); ++pi) {
                const ast::Param& pf = cc->caseDecl->payload[pi];
                if (positionalFieldIsSkip(pf)) continue;
                llvm::Value* fv = builder_.CreateExtractValue(
                    choiceVal, {static_cast<unsigned>(1 + ci),
                                static_cast<unsigned>(pi)}, "chser.pf");
                std::string wn(positionalFieldWireName(pf));
                llvm::Value* nameStr = lowerStringLiteral("\"" + wn + "\"", false);
                builder_.CreateCall(writeNameFn, {serPtr, nameStr});
                const sema::TypeRef* pty =
                    pf.type ? ctx_.resolvedType(*pf.type) : nullptr;
                emitSerializeField(fv, pty, serPtr);
            }
        }

        builder_.CreateCall(endFn, {serPtr});
        builder_.CreateBr(endBB);
    }

    builder_.SetInsertPoint(endBB);
}

// ---------------------------------------------------------------------------
// Emitter::emitChoiceDeserialize  §9.8 choice serialization (v0.200, #2046)
//
// Reads the `$case` discriminator FIRST (bz_binder_read_case_discriminator,
// which sets poly_preread_count so the per-case begin_object below returns the
// payload field count), dispatches on the discriminator string to select the
// case, reads the payload fields under the standard missing/duplicate/unknown
// rules, and builds the choice struct.  An undeclared discriminator string, or
// an object that does not lead with the discriminator, raises
// SerializationException (the leading-field faults live in the runtime helper;
// the undeclared-case fault is the no-match branch here).  Returns the
// constructed choice struct value.
// ---------------------------------------------------------------------------
llvm::Value* Emitter::emitChoiceDeserialize(const TypeSymbol* ts,
                                            llvm::Value* derPtr) {
    // fix #3185: same removal as emitChoiceSerialize — see the comment there
    // for the measured defect shape and the F9 reconstruction that makes the
    // imported path walkable.
    if (!ts) return nullptr;
    // fix #3140 follow-up: name this type in the backstop diagnostic.
    SerializationDiagScope diagScope(this, ts);
    llvm::Type* voidTy = llvm::Type::getVoidTy(llvm_);
    llvm::StructType* st = choiceStruct(ts);
    auto cases = choiceCasesOf(ts);

    // Result slot — the constructed choice value (each case block stores here).
    llvm::AllocaInst* resultSlot = entryAlloca(st);
    builder_.CreateStore(llvm::ConstantAggregateZero::get(st), resultSlot);

    // §9.8: read the leading "$case" discriminator FIRST (before any payload
    // binding).  The helper faults on a missing / mis-placed discriminator and
    // sets poly_preread_count = remaining-field-count.
    auto* readDiscFn = declareC("bz_binder_read_case_discriminator", ptrTy(),
                                {ptrTy()});
    llvm::Value* discPtr = builder_.CreateCall(readDiscFn, {derPtr}, "chdes.disc");
    emitPendingCheck();

    // Null discriminator → truncated / missing-discriminator fault.
    {
        llvm::Value* isNull = builder_.CreateICmpEQ(
            discPtr, llvm::ConstantPointerNull::get(ptrTy()), "chdes.disc.null");
        auto* nullBB = llvm::BasicBlock::Create(llvm_, "chdes.disc.nullbb", currentFn_);
        auto* liveBB = llvm::BasicBlock::Create(llvm_, "chdes.disc.livebb", currentFn_);
        builder_.CreateCondBr(isNull, nullBB, liveBB);
        builder_.SetInsertPoint(nullBB);
        emitThrowBuiltin("SerializationException");
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateUnreachable();
        builder_.SetInsertPoint(liveBB);
    }

    // Register discPtr for exception cleanup (mirrors emitPolyClassDeserialize).
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

    // One shared per-iteration field-name cleanup slot for the payload loops
    // (mirrors emitRecordDeserialize's namePtrSlot, #1635).  Only one case block
    // runs at run time, so a single shared slot is correct.
    llvm::AllocaInst* namePtrSlot = entryAlloca(ptrTy());
    {
        llvm::BasicBlock& eb = currentFn_->getEntryBlock();
        llvm::IRBuilder<> eb_b(&eb, std::next(namePtrSlot->getIterator()));
        eb_b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), namePtrSlot);
    }
    if (!disposeStack_.empty()) {
        Disposal nd{namePtrSlot, nullptr};
        nd.stringRelease = true;
        disposeStack_.back().push_back(nd);
    }

    auto* doneBB  = llvm::BasicBlock::Create(llvm_, "chdes.done",   currentFn_);
    auto* throwBB = llvm::BasicBlock::Create(llvm_, "chdes.badcase", currentFn_);

    // fix #4487 (stdlib §9.6 + spec §9.8, v0.306): equality-only dispatch
    // (`cmp == 0`, ignoreCase 0 — §9.8 compares names ORDINALLY). Moved off
    // `bz_string_compare`, which walked min(len) BYTES over a CODE-POINT
    // length (#535 class), so two distinct non-ASCII names sharing their
    // leading storage bytes matched. See emitRecordDeserialize's note for the
    // measurement; ASCII names are unaffected.
    auto* cmpFn = declareC("bz_str_compare_cp", i32(),
                           {ptrTy(), ptrTy(), i32()});
    auto* relFn = declareC("bz_string_release", voidTy, {ptrTy()});

    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        const sema::ChoiceCaseSymbol* cc = cases[ci];
        std::string wire(choiceCaseWireName(cc));
        llvm::Value* litStr = lowerStringLiteral("\"" + wire + "\"", false);
        // `discPtr` stays live until a matched case block releases it, and a
        // matched block is terminal (→ doneBB), so it is valid at every
        // comparison — the same premise the hoisted (data, len) loads rested on.
        llvm::Value* cmpRes = builder_.CreateCall(cmpFn,
            {discPtr, litStr, builder_.getInt32(0)}, "chdes.cmp");
        llvm::Value* isMatch = builder_.CreateICmpEQ(
            cmpRes, builder_.getInt32(0), "chdes.match");
        auto* caseBB = llvm::BasicBlock::Create(llvm_, "chdes.case", currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "chdes.next", currentFn_);
        builder_.CreateCondBr(isMatch, caseBB, nextBB);

        builder_.SetInsertPoint(caseBB);
        {
            // Discriminator selected; release it before reading payloads (a
            // payload read may throw — the discSlot cleanup covers the throw
            // window; nulling the slot prevents a double release on the normal
            // path).
            builder_.CreateCall(relFn, {discPtr});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), discSlot);

            // begin_object consumes poly_preread_count → the payload field count.
            auto* beginFn = declareC("bz_binder_begin_object", i32(), {ptrTy()});
            llvm::Value* payloadCount =
                builder_.CreateCall(beginFn, {derPtr}, "chdes.pcount");
            emitPendingCheck();

            unsigned payloadFieldIdx = static_cast<unsigned>(1 + ci);
            llvm::Type* payloadTy = st->getElementType(payloadFieldIdx);
            auto* payloadStruct = llvm::dyn_cast<llvm::StructType>(payloadTy);

            std::size_t np = (cc && cc->caseDecl) ? cc->caseDecl->payload.size() : 0;
            std::vector<llvm::AllocaInst*> slots(np, nullptr);
            std::vector<const sema::TypeRef*> ptys(np, nullptr);
            std::vector<std::string> wireNames(np);
            std::vector<bool> skip(np, false);
            // fix #2399 / §9.8 v0.235 + stdlib §2.15 v0.235 ("a duplicate
            // field name ... raises SerializationException unconditionally";
            // "missing required fields, duplicates, and type mismatches
            // remain unconditional errors regardless of the [unknown-field]
            // setting"): `required` records whether a MISSING occurrence is
            // tolerated (`serialized optional`) -- a seen-flag is now
            // allocated for every non-skip payload field regardless, since a
            // DUPLICATE occurrence is a structural fault even for an
            // optional payload.
            struct SeenEntry {
                std::size_t pi;
                llvm::AllocaInst* seenSlot;
                bool required;
            };
            std::vector<SeenEntry> seenFlags;

            for (std::size_t pi = 0; pi < np; ++pi) {
                const ast::Param& pf = cc->caseDecl->payload[pi];
                skip[pi] = positionalFieldIsSkip(pf);
                wireNames[pi] = std::string(positionalFieldWireName(pf));
                ptys[pi] = pf.type ? ctx_.resolvedType(*pf.type) : nullptr;
                if (skip[pi]) continue;
                if (!payloadStruct || pi >= payloadStruct->getNumElements()) continue;
                llvm::Type* et = payloadStruct->getElementType(
                    static_cast<unsigned>(pi));
                slots[pi] = entryAlloca(et);
                builder_.CreateStore(llvm::Constant::getNullValue(et), slots[pi]);
                // §9.8 / §serialized optional: a `serialized optional` payload is
                // exempt from the required-field check (missing → default(T)),
                // but NOT from the duplicate check (fix #2399) -- so a seen-flag
                // is allocated unconditionally here.
                bool optional = pf.serialized && pf.serialized->isOptional;
                llvm::AllocaInst* sf = entryAlloca(i8());
                builder_.CreateStore(builder_.getInt8(0), sf);
                seenFlags.push_back({pi, sf, !optional});
            }

            // fix #2398 / §9.8 / §28.3/§28.9: a dedicated scope guarding
            // already-populated payload slots against a LATER payload
            // field's read throwing — mirrors #2378's emitClassDeserialize
            // fix (Bug B) and emitRecordDeserialize's matching fix above.
            // A choice's payload fields are record-admissible components
            // (§9.8 "Choice serialization": "every payload field's type is
            // serialization-eligible and record-admissible ... the same
            // Deserialize-has-no-owner argument as for class-typed record
            // fields"), so — exactly as for records — there is no class
            // half to mirror; only string-typed and nested-record-typed
            // (one level, via the #1747 emitRecordStringFieldReleases
            // reuse — see emitRecordDeserialize's matching comment above
            // for the full rationale and its documented narrow scope)
            // payload slots are registered. Pushed before any payload is
            // read; popped WITHOUT disposal once every required payload is
            // confirmed present (below, just before the case's value is
            // constructed) — by then each slot's value is about to be
            // consumed into the constructed choice. Scoped PER CASE (this
            // block runs once per declared case; only one ever executes at
            // runtime, but each has its own `slots`), unlike the record/
            // class fix's single scope for the whole function.
            disposeStack_.emplace_back();
            for (std::size_t pi = 0; pi < np; ++pi) {
                if (skip[pi] || !slots[pi] || !ptys[pi]) continue;
                const auto* pty = ptys[pi];
                if (pty->kind == sema::TypeRef::Kind::Primitive &&
                    pty->primitive == sema::PrimitiveType::String) {
                    Disposal sd{slots[pi], nullptr};
                    sd.stringRelease = true;
                    disposeStack_.back().push_back(sd);
                } else if (isRecordWithStringFieldsTypeRef(pty)) {
                    Disposal rd{slots[pi], pty->declaration};
                    rd.recordStringFields = true;
                    // fix #3945 (§9.7 / §28.9 / §14.1, v0.294): carry `pty`
                    // itself (genericArgs included) through to the release —
                    // see Disposal::clsRef (LlvmBackend_impl.hpp).
                    rd.clsRef = pty;
                    disposeStack_.back().push_back(rd);
                } else if (!pty->nullable &&
                           pty->kind == sema::TypeRef::Kind::Named &&
                           pty->declaration &&
                           pty->declaration->kind == ast::TypeDeclKind::Choice &&
                           choiceHasStringFields(pty->declaration)) {
                    // fix #2425 / §9.8 / §28.3/§28.9 / §35.4: a choice
                    // payload field may itself be a value-like CHOICE
                    // (record-admissible per §9.8, nested at any depth) —
                    // see the matching branch in emitRecordDeserialize above
                    // for the full rationale.
                    Disposal cd{slots[pi], pty->declaration};
                    cd.choiceStringFields = true;
                    disposeStack_.back().push_back(cd);
                } else if (!pty->nullable &&
                           pty->kind == sema::TypeRef::Kind::InlineArray &&
                           pty->arrayElement &&
                           pty->arraySizeValue >= 0 &&
                           inlineArrayElementCarriesString(pty->arrayElement)) {
                    // fix #2326 / §9.8 v0.228: a non-nullable INLINE-ARRAY
                    // choice payload whose element type carries a string
                    // — the array-side mirror of the choiceStringFields
                    // arm just above. See the matching record branch in
                    // emitRecordDeserialize for the full rationale.
                    Disposal ad{slots[pi], nullptr};
                    ad.inlineArrayStringFields = true;
                    ad.inlineArrayElemType = pty;
                    disposeStack_.back().push_back(ad);
                }
            }

            // Payload-read loop (payloadCount iterations).
            auto* loopHdr  = llvm::BasicBlock::Create(llvm_, "chdes.loop.hdr",  currentFn_);
            auto* loopBody = llvm::BasicBlock::Create(llvm_, "chdes.loop.body", currentFn_);
            auto* loopEnd  = llvm::BasicBlock::Create(llvm_, "chdes.loop.end",  currentFn_);
            llvm::AllocaInst* idxSlot = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(0), idxSlot);
            builder_.CreateBr(loopHdr);

            builder_.SetInsertPoint(loopHdr);
            llvm::Value* idx = builder_.CreateLoad(i32(), idxSlot, "chdes.idx");
            llvm::Value* more = builder_.CreateICmpSLT(idx, payloadCount, "chdes.more");
            builder_.CreateCondBr(more, loopBody, loopEnd);

            builder_.SetInsertPoint(loopBody);
            auto* readNameFn = declareC("bz_binder_read_field_name", ptrTy(), {ptrTy()});
            llvm::Value* namePtr = builder_.CreateCall(readNameFn, {derPtr}, "chdes.fname");
            emitPendingCheck();
            builder_.CreateStore(namePtr, namePtrSlot);

            // fix #2404 / §9.8 / §28.9: same OOM-residual null-without-
            // exception gap as emitRecordDeserialize's matching fix above
            // (see its comment for the full rationale) -- the ordinary
            // truncated-input path is already caught by emitPendingCheck()
            // just above (fix #2403); only a raw malloc/calloc failure
            // inside bz_binder_read_bz_string can still leave namePtr null
            // here with no pending exception, and it is dereferenced
            // unguarded a few lines down. No manual per-slot dispose loop
            // needed: the #2398 disposeStack_ scope pushed above (before
            // this payload loop, per case) already registers every
            // already-filled payload slot for automatic release, swept by
            // emitThrowBuiltin's emitUnwindDisposals() below.
            {
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    namePtr, llvm::ConstantPointerNull::get(ptrTy()),
                    "chdes.fname.isnull");
                auto* nullBB = llvm::BasicBlock::Create(
                    llvm_, "chdes.fname.null", currentFn_);
                auto* liveBB = llvm::BasicBlock::Create(
                    llvm_, "chdes.fname.live", currentFn_);
                builder_.CreateCondBr(isNull, nullBB, liveBB);
                builder_.SetInsertPoint(nullBB);
                emitThrowBuiltin("SerializationException");
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateUnreachable();
                builder_.SetInsertPoint(liveBB);
            }

            auto* skipBB  = llvm::BasicBlock::Create(llvm_, "chdes.skip",  currentFn_);
            auto* afterBB = llvm::BasicBlock::Create(llvm_, "chdes.after", currentFn_);

            for (std::size_t pi = 0; pi < np; ++pi) {
                if (skip[pi] || !slots[pi]) continue;
                llvm::Value* litStr2 = lowerStringLiteral(
                    "\"" + wireNames[pi] + "\"", false);
                // fix #4487: the choice's PAYLOAD-field name dispatch, the
                // second user of this function's `cmpFn` — same equality test
                // on the same defect, moved with it.
                llvm::Value* cmpR = builder_.CreateCall(cmpFn,
                    {namePtr, litStr2, builder_.getInt32(0)}, "chdes.pcmp");
                llvm::Value* isM = builder_.CreateICmpEQ(
                    cmpR, builder_.getInt32(0), "chdes.pmatch");
                auto* rdBB = llvm::BasicBlock::Create(llvm_, "chdes.rd", currentFn_);
                auto* ndBB = llvm::BasicBlock::Create(llvm_, "chdes.nd", currentFn_);
                builder_.CreateCondBr(isM, rdBB, ndBB);

                builder_.SetInsertPoint(rdBB);

                // fix #2399 / §9.8 v0.235 + stdlib §2.15 v0.235 (quoted at
                // the seenFlags allocation above): a payload field matched a
                // SECOND time by the wire (its seen-flag already 1) is a
                // duplicate field name -- raised BEFORE reading anything for
                // this occurrence, so the value already sitting in
                // slots[pi] from the first occurrence is left for the
                // #2398 per-case disposal scope (pushed above) to release on
                // the emitThrowBuiltin unwind below -- mirrors
                // emitClassDeserialize/emitRecordDeserialize's identical fix.
                {
                    llvm::AllocaInst* mySeenSlot = nullptr;
                    for (const auto& se : seenFlags) {
                        if (se.pi == pi) { mySeenSlot = se.seenSlot; break; }
                    }
                    if (mySeenSlot) {
                        llvm::Value* alreadySeen = builder_.CreateLoad(
                            i8(), mySeenSlot, "chdes.dup.seen");
                        llvm::Value* isDup = builder_.CreateICmpNE(
                            alreadySeen, builder_.getInt8(0), "chdes.dup.isdup");
                        auto* dupBB = llvm::BasicBlock::Create(
                            llvm_, "chdes.dup", currentFn_);
                        auto* freshBB = llvm::BasicBlock::Create(
                            llvm_, "chdes.fresh", currentFn_);
                        builder_.CreateCondBr(isDup, dupBB, freshBB);

                        builder_.SetInsertPoint(dupBB);
                        emitThrowBuiltin("SerializationException");
                        if (!builder_.GetInsertBlock()->getTerminator())
                            builder_.CreateUnreachable();

                        builder_.SetInsertPoint(freshBB);
                    }
                }
                llvm::Value* fv = emitDeserializeField(ptys[pi], derPtr);
                if (fv && slots[pi] && payloadStruct &&
                    pi < payloadStruct->getNumElements() &&
                    fv->getType() == payloadStruct->getElementType(
                        static_cast<unsigned>(pi)))
                    builder_.CreateStore(fv, slots[pi]);
                emitPendingCheck();
                for (const auto& se : seenFlags)
                    if (se.pi == pi) {
                        builder_.CreateStore(builder_.getInt8(1), se.seenSlot);
                        break;
                    }
                builder_.CreateCall(relFn, {namePtr});
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), namePtrSlot);
                builder_.CreateBr(afterBB);

                builder_.SetInsertPoint(ndBB);
            }
            // Unknown field → SkipValue (unknown-field policy in the deserializer).
            builder_.CreateBr(skipBB);
            builder_.SetInsertPoint(skipBB);
            auto* skipFn = declareC("bz_binder_skip_value", voidTy, {ptrTy()});
            builder_.CreateCall(skipFn, {derPtr});
            emitPendingCheck();
            builder_.CreateCall(relFn, {namePtr});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), namePtrSlot);
            builder_.CreateBr(afterBB);

            builder_.SetInsertPoint(afterBB);
            llvm::Value* nidx = builder_.CreateAdd(idx, builder_.getInt32(1),
                                                   "chdes.idx1");
            builder_.CreateStore(nidx, idxSlot);
            builder_.CreateBr(loopHdr);

            builder_.SetInsertPoint(loopEnd);
            auto* endObjFn = declareC("bz_binder_end_object", voidTy, {ptrTy()});
            builder_.CreateCall(endObjFn, {derPtr});
            emitPendingCheck();

            // §9.8: a required payload field (present neither in the input nor as
            // `serialized optional`) that was not seen is a structural fault.
            // fix #2399: seenFlags now also holds entries for OPTIONAL payload
            // fields (so the dispatch loop's duplicate check above can see
            // them); skip those here -- missing stays fine for an optional
            // payload field.
            for (const auto& se : seenFlags) {
                if (!se.required) continue;
                llvm::Value* seen = builder_.CreateLoad(i8(), se.seenSlot, "chdes.seen");
                llvm::Value* miss = builder_.CreateICmpEQ(
                    seen, builder_.getInt8(0), "chdes.nseen");
                auto* missBB = llvm::BasicBlock::Create(llvm_, "chdes.miss", currentFn_);
                auto* okBB   = llvm::BasicBlock::Create(llvm_, "chdes.mok",  currentFn_);
                builder_.CreateCondBr(miss, missBB, okBB);
                builder_.SetInsertPoint(missBB);
                emitThrowBuiltin("SerializationException");
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateUnreachable();
                builder_.SetInsertPoint(okBB);
            }

            // fix #2398 / §9.8 / §28.3/§28.9: pop the payload-disposal scope
            // pushed above. Every required payload is now confirmed present
            // and construction is about to begin, consuming each slot's
            // value into the constructed choice — nothing in this scope
            // should be disposed on this (normal) path, so pop it WITHOUT
            // invoking its disposals (mirrors the record/class fix's pop).
            disposeStack_.pop_back();

            // Build the choice value: tag = ci, payload assembled from the slots.
            llvm::Value* payloadVal = llvm::Constant::getNullValue(payloadTy);
            if (payloadStruct) {
                for (std::size_t pi = 0; pi < np; ++pi) {
                    if (pi >= payloadStruct->getNumElements()) break;
                    llvm::Value* fv;
                    if (slots[pi])
                        fv = builder_.CreateLoad(
                            payloadStruct->getElementType(static_cast<unsigned>(pi)),
                            slots[pi]);
                    else  // skip / no slot → default(T).
                        fv = llvm::Constant::getNullValue(
                            payloadStruct->getElementType(static_cast<unsigned>(pi)));
                    payloadVal = builder_.CreateInsertValue(
                        payloadVal, fv, {static_cast<unsigned>(pi)});
                }
            }
            llvm::Value* whole = llvm::Constant::getNullValue(st);
            whole = builder_.CreateInsertValue(
                whole, builder_.getInt32(static_cast<int>(ci)), {0u});
            whole = builder_.CreateInsertValue(whole, payloadVal, {payloadFieldIdx});
            builder_.CreateStore(whole, resultSlot);
            builder_.CreateBr(doneBB);
        }

        builder_.SetInsertPoint(nextBB);
    }

    // No case matched: undeclared discriminator → SerializationException (§9.8).
    builder_.CreateBr(throwBB);
    builder_.SetInsertPoint(throwBB);
    builder_.CreateCall(relFn, {discPtr});
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), discSlot);
    emitThrowBuiltin("SerializationException");
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateUnreachable();

    builder_.SetInsertPoint(doneBB);
    return builder_.CreateLoad(st, resultSlot, "chdes.result");
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
    // fix #3140 (family F2 of #3152): the base's §9.8 surface and registry,
    // from the symbol.
    if (!baseTs || sema::serializedClassShape(baseTs).empty()) return;
    const auto& polyCases = sema::serializedPolyCasesOf(baseTs);
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
        // fix #3140 (family F2 of #3152): the registered case's resolved symbol
        // and its §9.8 surface, both off the base's symbol-side registry.
        const TypeSymbol* pcTs = pc.type;
        if (!pcTs || sema::serializedClassShape(pcTs).empty()) continue;

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
    // fix #3140 (family F2 of #3152): the base's §9.8 surface and registry,
    // from the symbol.
    if (!baseTs || sema::serializedClassShape(baseTs).empty())
        return llvm::ConstantPointerNull::get(ptrTy());
    const auto& polyCases = sema::serializedPolyCasesOf(baseTs);
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
    // fix #4487 (stdlib §9.6 + spec §9.8, v0.306): equality-only dispatch
    // (`cmp == 0`, ignoreCase 0 — §9.8 compares names ORDINALLY). Moved off
    // `bz_string_compare`, which walked min(len) BYTES over a CODE-POINT
    // length (#535 class), so two distinct non-ASCII names sharing their
    // leading storage bytes matched. See emitRecordDeserialize's note for the
    // measurement; ASCII names are unaffected.
    auto* cmpFn = declareC("bz_str_compare_cp", i32(),
                           {ptrTy(), ptrTy(), i32()});

    for (const auto& pc : polyCases) {
        // fix #3140 (family F2 of #3152): the registered case's resolved symbol
        // and its §9.8 surface, both off the base's symbol-side registry.
        const TypeSymbol* pcTs = pc.type;
        if (!pcTs || sema::serializedClassShape(pcTs).empty()) continue;

        // Compare discriminator string with this case's literal.
        llvm::Value* litStr = lowerStringLiteral(
            "\"" + std::string(pc.discriminator) + "\"", false);
        llvm::Value* cmpRes = builder_.CreateCall(cmpFn,
            {discPtr, litStr, builder_.getInt32(0)}, "poly.cmp");
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

    // fix #3140 follow-up: name this type in the backstop diagnostic.
    SerializationDiagScope diagScope(this, ts);
    // Detect graph mode: true for `serialized graph` and `serialized cyclic`.
    // fix #3140 (family F2 of #3152): the derived shape, from the symbol.
    const std::string_view shape = sema::serializedClassShape(ts);
    bool isGraphMode = shape == "graph" || shape == "cyclic";

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

    // fix #3140 follow-up: name this type in the backstop diagnostic.
    SerializationDiagScope diagScope(this, ts);
    // Detect graph mode.
    // fix #3140 (family F2 of #3152): the derived shape, from the symbol.
    const std::string_view shapeD = sema::serializedClassShape(ts);
    bool isGraphMode = shapeD == "graph" || shapeD == "cyclic";

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
    // errors."  For each non-skip component allocate an i8 seen-flag alloca
    // (entry-block init to 0); the flag is set to 1 when the field is matched
    // in the loop.  After EndObject, any REQUIRED flag still 0 means a
    // required component was absent → raise SerializationException.
    //
    // Nullable T? fields without `serialized optional` are REQUIRED: nullability
    // does not confer optionality.  Only the `serialized optional` clause exempts
    // a field from the missing-field check (§9.8: "serialized optional is the
    // per-field missing-field tolerant-mode opt-in").
    //
    // fix #2399 / §9.8 v0.235 ("A missing required component ..., a
    // **duplicate** field name, a type mismatch, or a name not matching any
    // enum constant raises SerializationException unconditionally -- these
    // are structural faults in the input regardless of policy"; stdlib
    // §2.15 v0.235 Errors: "missing required fields, duplicates, and type
    // mismatches remain unconditional errors regardless of the [unknown-
    // field] setting"): a seen-flag is now allocated for EVERY non-skip
    // component, not just required ones -- a duplicate `serialized optional`
    // field is equally an unconditional structural fault, so the per-field
    // dispatch loop below needs a flag to re-check even for an optional
    // component. `required` gates ONLY the missing-field check further down
    // (optional: missing is still fine); the duplicate check the dispatch
    // loop performs applies to every entry here regardless of `required`.
    struct SeenEntry {
        std::size_t compIdx;      // index into comps[]
        std::string declName;     // for the error message
        llvm::AllocaInst* slot;   // i8 flag: 0=unseen, 1=seen
        bool required;            // false for `serialized optional` components
    };
    std::vector<SeenEntry> seenFlags;
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].isSkip) continue;      // skip excluded from wire
        // Seen-flag for every non-skip component (required or optional):
        // allocate (hoisted to the entry block, as always) but zero it at the
        // CURRENT insertion point, not forced into the entry block.
        //
        // fix #2399 adversarial self-pass (regression caught by the existing
        // #2325 `repeated_roundtrip_no_accumulating_leak` test, which calls
        // `Widget.Deserialize` 50x in a loop around ONE inlined copy of this
        // function's generated code): unlike namePtrSlot/slots[] below --
        // which fix #669 deliberately zero-inits in the entry block because
        // a DIFFERENT code path's disposal sweep can read them before this
        // invocation's own code has run at all -- a seen-flag is consulted
        // ONLY by this SAME straight-line invocation's own dispatch loop and
        // missing-field check, never externally. Forcing its zero-init into
        // the entry block (as originally written) makes it run ONCE per
        // FUNCTION rather than once per logical Deserialize call: inside a
        // loop calling Deserialize repeatedly, the flag set to 1 on
        // iteration 1 stays 1 forever after, so iteration 2's very first
        // field read looks like a "duplicate" of iteration 1's already-
        // consumed-and-freed value -- a false-positive throw whose unwind
        // then bz_string_release's a STALE (already-freed) slots[] pointer
        // left over from iteration 1 -> a use-after-free crash (confirmed:
        // reverting this one hunk while keeping the rest of the #2399 fix
        // reproduces the crash; restoring it fixes it, exit 42 clean under
        // ASan). Resetting at the current insertion point instead makes the
        // flag correctly per-invocation, matching emitRecordDeserialize's
        // and emitChoiceDeserialize's seenFlags loops, which never had this
        // bug (they always used a plain builder_.CreateStore here). As a
        // side effect this also fixes the SAME staleness in fix #668's own
        // missing-required-field check for a class Deserialize called
        // repeatedly in a loop -- a latent, pre-existing gap this happens to
        // repair, not a new behavior change for any single (non-looped) call.
        llvm::AllocaInst* sf = entryAlloca(i8());
        builder_.CreateStore(builder_.getInt8(0), sf);
        seenFlags.push_back({i, comps[i].declName, sf, !comps[i].isOptional});
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

    // fix #2378 / §9.8 / §28.3/§28.9: a dedicated scope guarding already-
    // populated owned-class/string component slots against a LATER field's
    // read throwing. Pushed here (before any field is read); each class- or
    // string-typed component's slot is registered into it, once, up front.
    // Popped WITHOUT disposal once construction begins (below, after the
    // missing-required-field check), since by then every slot's value is
    // either about to be consumed into the constructed object or the
    // function has already unwound past this point entirely.
    //
    // fix #2460 (v0.235): this scope is now the SOLE disposal mechanism for
    // every throw site inside this function (the truncated-field-name errBB
    // block and the missing-required-field block, both below, used to walk
    // `comps`/`slots` a second and third time with their OWN manual dispose
    // loops immediately before their throw — removed; see their own comments
    // for why duplicating this scope's classification there, slightly
    // differently each time, produced a real heap-use-after-free on a
    // graph-mode back-edge slot). Every throw in this function goes through
    // `emitThrowBuiltin`, which unconditionally calls `emitUnwindDisposals()`
    // and sweeps this scope (it is armed unconditionally, before any field is
    // read, regardless of which specific throw site fires later) — so one
    // correctly-classified registration, not three independently-maintained
    // copies of the same classification, is what disposes every
    // already-populated slot on every path out of this loop.
    //
    // Without this: a component slot populated by an EARLIER successful
    // field read (e.g. `First: string` read before `Second: owned Leaf`)
    // had NO disposal registered anywhere. A LATER field's read that
    // recurses into ANOTHER emitClassDeserialize/emitDeserializeField
    // invocation and throws from deep inside (its own
    // bz_binder_begin_object / field-name-null check, already correctly
    // pending-checked per the emitDeserializeField fix above) unwinds
    // straight past this function's own manual disposal loops (which only
    // run on THIS class's OWN field-name-null / missing-required-field
    // paths, not a nested recursive throw) — orphaning the already-read
    // value (the reported #2378 follow-up string leak,
    // `bz_binder_read_bz_string`/`bz_binder_read_string`).
    //
    // fix #2397 / §9.8 / §28.3/§28.9/§28.13 (v0.235): #2378 scoped this
    // registration to tree mode only (`!isGraphMode`), because a graph-mode
    // component slot is not uniformly "a plain class instance or a string" —
    // graph mode's per-field value shapes (mirrored from the ACTUAL dispatch
    // a few hundred lines below, in the per-field loop over `comps`) each
    // need a DIFFERENT release, dispatched in the SAME order and on the SAME
    // predicates that loop uses, so the two decisions stay in lockstep:
    //   1. Shared<C> identity participant (handledSharedElementClass): the
    //      slot holds a bz_shared_t* HANDLE (bz_shared_new / bz_shared_clone)
    //      — released via bz_shared_release (§28.13 / stdlib §2.16's `using
    //      var` Shared<T> path, reused here via the `sharedHandle` Disposal
    //      flag). Reusing the generic vtable dispatch would misinterpret the
    //      handle's layout as a class instance — a wrong-dispatch regression.
    //   2. owned Weak<C> back-reference (handledWeakElementClass, gated on
    //      comp.isOwned exactly as the real dispatch requires): the slot
    //      holds a bz_weak_t* HANDLE (bz_shared_downgrade) — released via
    //      bz_weak_release (`weakHandle` Disposal flag). Same wrong-dispatch
    //      hazard as Shared if misrouted through the generic path.
    //   3. plain-borrow back-edge (comp.isBackEdge — a non-owned class-typed
    //      field, e.g. `Alias: Slot?`): the slot holds a BORROWED pointer
    //      resolved via bz_binder_graph_resolve_tag. This component owns
    //      NOTHING (the object is owned by whichever `owned`/Shared<C>
    //      component first constructed it), so NO disposal is registered —
    //      registering the generic vtable dispatch here would premature-free
    //      a value another live component/handle still references.
    //   4. owned-class-in-graph-mode (comp.isOwned && a plain Named class,
    //      not Shared/Weak — e.g. `owned Target: Slot`): the slot holds a
    //      plain owned class instance from emitClassDeserialize /
    //      emitPolyClassDeserialize, exactly like a tree-mode owned-class
    //      component — falls through to the SAME generic-vtable-dispatch
    //      branch tree mode already used (no graph-mode-specific handling
    //      needed; it was excluded before only by the blanket `!isGraphMode`
    //      gate this fix removes).
    //   A plain string component is likewise no longer excluded in graph
    //   mode: a graph-shaped class may carry ordinary string fields
    //   alongside its identity-tagged ones (strings are never
    //   identity-tagged themselves, §9.8), so it needs the SAME
    //   `stringRelease` disposal tree mode already registers.
    // Order matches the real per-field dispatch's own `if`/`else if` chain
    // (Shared, then owned-Weak, then back-edge, then owned-class) so a
    // component can never be double-classified.
    disposeStack_.emplace_back();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].isSkip || !slots[i] || !comps[i].ty) continue;
        const auto& comp = comps[i];
        const auto* cty = comp.ty;
        if (isGraphMode && handledSharedElementClass(cty)) {
            // Shared<C> identity participant — bz_shared_release, null-
            // guarded, on unwind only (LlvmBackend_Disposal.cpp's
            // emitDisposals `it->sharedHandle` branch).
            Disposal d{slots[i], nullptr};
            d.unwindOnly = true;
            d.sharedHandle = true;
            disposeStack_.back().push_back(d);
        } else if (isGraphMode && comp.isOwned && handledWeakElementClass(cty)) {
            // owned Weak<C> back-reference — bz_weak_release, null-guarded,
            // on unwind only.
            Disposal d{slots[i], nullptr};
            d.unwindOnly = true;
            d.weakHandle = true;
            disposeStack_.back().push_back(d);
        } else if (isGraphMode && comp.isBackEdge) {
            // Plain-borrow back-edge: owns nothing — no disposal registered.
        } else if (cty->kind == sema::TypeRef::Kind::Named && cty->declaration &&
                   cty->declaration->kind == ast::TypeDeclKind::Class) {
            // Tree-mode class component, OR graph mode's owned-class-in-
            // graph-mode component (§9.8 tier 3): a plain owned class
            // instance, disposed via the generic vtable dispatch.
            //
            // unwindOnly: skipped entirely on normal exit (belt-and-
            // suspenders — this scope is popped without disposal on the
            // normal path anyway), null-guarded generic vtable dispatch
            // on unwind (LlvmBackend_Disposal.cpp's emitDisposals).
            Disposal cd{slots[i], cty->declaration};
            cd.unwindOnly = true;
            disposeStack_.back().push_back(cd);
        } else if (cty->kind == sema::TypeRef::Kind::Primitive &&
                   cty->primitive == sema::PrimitiveType::String) {
            Disposal sd{slots[i], nullptr};
            sd.stringRelease = true;
            disposeStack_.back().push_back(sd);
        }
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
    // fix #2403 follow-up / §2.15: bz_binder_read_field_name's binary path can
    // now raise DIRECTLY (bz_binder_read_bz_string calls bz_ser_throw_at_binary
    // on truncation as of the #2403 fix, matching every other binary reader) —
    // check for that pending exception BEFORE the manual null-check-and-throw
    // block below, mirroring every other read call in this function (e.g.
    // bz_binder_begin_object above). Without this, the manual block's own
    // unconditional emitThrowBuiltin() call a few lines down would SUPERSEDE
    // the #2403 exception with a fresh, message-less one — orphaning/leaking
    // the first exception object, exactly the #2378 Bug A pattern this
    // function was already fixed against for every OTHER read in its loop.
    // The pre-existing disposeStack_ scope pushed above (before this loop
    // starts) is still active here, so emitPendingCheck()'s unwind still
    // disposes every already-filled component slot correctly.
    emitPendingCheck();

    // §9.8 / #550: truncated input — bz_binder_read_field_name returns null
    // WITHOUT a pending exception (reached only for the OOM allocation-failure
    // paths inside bz_binder_read_bz_string, which still return null silently
    // — a truncation-caused null is now caught by emitPendingCheck() above).
    // Raise SerializationException; already-filled slots are disposed by
    // emitThrowBuiltin's own emitUnwindDisposals() call, which sweeps the
    // component-disposal scope pushed near the top of this function.
    //
    // fix #2460 / §9.8 / §28.3/§28.9/§28.13 (v0.235): this block used to walk
    // `comps`/`slots` itself and manually dispose+null every non-null class/
    // string slot BEFORE the throw, so the (#2378-introduced) disposal-scope
    // sweep inside emitThrowBuiltin would see null and safely no-op. That
    // manual walk classified a slot as disposable by `ty->kind == Named &&
    // ty->declaration && Class` alone — true for a genuinely owned class
    // component AND for a graph-mode plain-borrow BACK-EDGE component (e.g.
    // `Alias: Slot?`), whose resolved type is a real class TypeSymbol just
    // like an owned field's. Unlike `Shared<T>`/`Weak<T>` (no `declaration`,
    // correctly skipped by this same check, if left un-disposed — a leak, not
    // a crash), a back-edge slot holds a BORROWED pointer to an object an
    // EARLIER owned/Shared component already registered as ITS OWN slot. This
    // walk did not exclude it, so disposing both `Target` (owned) and `Alias`
    // (its back-edge alias, the SAME pointer) double-freed the object —
    // empirically confirmed as a heap-use-after-free (#2460).
    //
    // The fix is not a third exclusion check bolted onto this walk: #2397
    // (a few dozen lines above, the disposeStack_ scope pushed near the top
    // of this function) already built the CORRECT per-component dispatch —
    // Shared<C> -> bz_shared_release, owned Weak<C> -> bz_weak_release,
    // back-edge -> no disposal (owns nothing), owned-class/string -> the
    // same mechanisms this walk used — and registers it UNCONDITIONALLY,
    // before ANY field is read, so it is already armed here regardless of
    // which slots happen to be populated when this specific throw fires.
    // Since `emitThrowBuiltin` unconditionally calls `emitUnwindDisposals()`,
    // that scope's sweep is FULLY SUFFICIENT for this throw site — a second,
    // manual walk duplicating the same classification (and getting it wrong
    // for back-edges) added risk without adding coverage. Deleted rather than
    // patched: three near-identical dispose loops in one function (this one,
    // the missing-required-field one below, and #2397's own registration) is
    // the shape that let the back-edge exclusion drift out of sync in the
    // first place; collapsing to the ONE already-fixed, already-tested
    // mechanism is the fix, not a fourth parallel patch.
    {
        llvm::Value* isNull = builder_.CreateICmpEQ(
            namePtr, llvm::ConstantPointerNull::get(ptrTy()), "cdeser.nfname");
        auto* errBB  = llvm::BasicBlock::Create(llvm_, "cdeser.trunc", currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "cdeser.cont",  currentFn_);
        builder_.CreateCondBr(isNull, errBB, contBB);

        builder_.SetInsertPoint(errBB);
        // Raise SerializationException (sets bz_exc, unwinds, branches to
        // unwindBB — emitUnwindDisposals() along the way disposes every
        // already-filled component slot correctly, per the #2397/#2460
        // scope above).
        emitThrowBuiltin("SerializationException");
        // emitThrowBuiltin branches to unwindBB; add unreachable as fallback.
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateUnreachable();

        builder_.SetInsertPoint(contBB);
        // Record the live namePtr so emitUnwindDisposals can release it if a
        // nested deserialization throws before the normal match/skip branch.
        builder_.CreateStore(namePtr, namePtrSlot);
    }

    // fix #4487 (stdlib §9.6 + spec §9.8, v0.306): equality-only dispatch
    // (`cmp == 0`, ignoreCase 0 — §9.8 compares names ORDINALLY). Moved off
    // `bz_string_compare`, which walked min(len) BYTES over a CODE-POINT
    // length (#535 class), so two distinct non-ASCII names sharing their
    // leading storage bytes matched. See emitRecordDeserialize's note for the
    // measurement; ASCII names are unaffected.
    auto* cmpFn = declareC("bz_str_compare_cp", i32(),
                           {ptrTy(), ptrTy(), i32()});

    auto* skipBB  = llvm::BasicBlock::Create(llvm_, "cdeser.skip",  currentFn_);
    auto* afterBB = llvm::BasicBlock::Create(llvm_, "cdeser.after", currentFn_);

    for (unsigned ci = 0; ci < comps.size(); ++ci) {
        const ClassComponent& comp = comps[ci];
        if (comp.isSkip || !slots[ci]) continue;

        const std::string& wn = comp.wireName;
        llvm::Value* litStr = lowerStringLiteral("\"" + wn + "\"", false);

        llvm::Value* cmpRes = builder_.CreateCall(cmpFn,
            {namePtr, litStr, builder_.getInt32(0)}, "cdeser.cmp");
        llvm::Value* isMatch = builder_.CreateICmpEQ(
            cmpRes, builder_.getInt32(0), "cdeser.match");

        auto* readBB = llvm::BasicBlock::Create(llvm_, "cdeser.rd", currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "cdeser.nd", currentFn_);
        builder_.CreateCondBr(isMatch, readBB, nextBB);

        builder_.SetInsertPoint(readBB);

        // fix #2399 / §9.8 v0.235 + stdlib §2.15 v0.235 (quoted above the
        // seenFlags allocation): a component matched a SECOND time by the
        // wire (its seen-flag is already 1) is a duplicate field name -- an
        // unconditional structural fault, raised BEFORE reading anything for
        // this occurrence (§9.8: "a duplicate field name ... raises
        // SerializationException unconditionally"). Without this check the
        // dispatch below would silently overwrite slots[ci] with the new
        // value and drop the earlier one, which the disposeStack_ scope
        // pushed above never gets a chance to release (issue #2399's
        // reported 73-byte leak: the FIRST value silently orphaned while the
        // SECOND "wins"). Raising here instead lets that SAME scope's unwind
        // sweep (emitThrowBuiltin -> emitUnwindDisposals) dispose the
        // earlier-read value that IS already sitting in slots[ci] -- no
        // separate manual release needed. namePtrSlot already holds this
        // iteration's namePtr (stored in contBB above) and is registered for
        // stringRelease, so it too is released by the same unwind sweep.
        {
            llvm::AllocaInst* mySeenSlot = nullptr;
            for (const auto& se : seenFlags) {
                if (se.compIdx == ci) { mySeenSlot = se.slot; break; }
            }
            if (mySeenSlot) {
                llvm::Value* alreadySeen = builder_.CreateLoad(
                    i8(), mySeenSlot, "cdeser.dup.seen");
                llvm::Value* isDup = builder_.CreateICmpNE(
                    alreadySeen, builder_.getInt8(0), "cdeser.dup.isdup");
                auto* dupBB = llvm::BasicBlock::Create(
                    llvm_, "cdeser.dup", currentFn_);
                auto* freshBB = llvm::BasicBlock::Create(
                    llvm_, "cdeser.fresh", currentFn_);
                builder_.CreateCondBr(isDup, dupBB, freshBB);

                builder_.SetInsertPoint(dupBB);
                emitThrowBuiltin("SerializationException");
                if (!builder_.GetInsertBlock()->getTerminator())
                    builder_.CreateUnreachable();

                builder_.SetInsertPoint(freshBB);
            }
        }
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
                // fix #2396 / §9.8 v0.235 (Bug-A pattern): the JSON path can
                // raise inside bz_jder_parse_int on a malformed/truncated
                // $id/$ref tag_id while still returning a "successful" marker
                // (tag_id left at 0) — propagate the pending exception before
                // acting on that bogus marker/tag, mirroring
                // bz_binder_read_case_discriminator's call sites in this file.
                emitPendingCheck();
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
                // fix #2396 / §9.8 v0.235 (Bug-A pattern): see the Shared<C>
                // branch above — propagate a pending exception from a
                // malformed/truncated $ref tag_id before resolving it.
                emitPendingCheck();
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
                // (marker VALUE not checked beyond this — trust wire format
                // is REF here — but fix #2396 / §9.8 v0.235: a malformed/
                // truncated $ref tag_id can leave a pending exception behind
                // a "successful" marker (tag_id left at 0), so check for
                // that pending exception before resolving the bogus tag.)
                builder_.CreateCall(readMarkerFn, {derPtr, tagSlot});
                emitPendingCheck();
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
                // fix #2396 / §9.8 v0.235 (Bug-A pattern): see the Shared<C>
                // branch above — a malformed/truncated $id tag_id on this
                // INLINE path can leave a pending exception behind a
                // "successful" marker; propagate it before marking the tag
                // in-progress and recursing into the (now-desynced) stream.
                emitPendingCheck();
                llvm::Value* tagId = builder_.CreateLoad(i32(), tagSlot, "cdeser.ot.tag");
                // Mark the tag as in-progress (cycle sentinel).
                auto* markFn = declareC("bz_binder_graph_mark_in_progress",
                                        voidTy, {ptrTy(), i32()});
                builder_.CreateCall(markFn, {derPtr, tagId});
                // §9.8 tier 4: deserialize the inline object body; if the component
                // type has a polymorphic registry, dispatch via "$type" discriminator.
                const TypeSymbol* compTs = comp.ty->declaration;
                // fix #3140 (family F2 of #3152): the component's registry,
                // from the symbol.
                llvm::Value* childPtr =
                    !sema::serializedPolyCasesOf(compTs).empty()
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
    // fix #2399: seenFlags now also holds entries for OPTIONAL components (so
    // the dispatch loop's duplicate check above can see them); skip those
    // here -- "missing" stays fine for an optional field, only `required`
    // entries feed this check.
    for (const auto& se : seenFlags) {
        if (!se.required) continue;
        llvm::Value* seen = builder_.CreateLoad(i8(), se.slot, "cdeser.seen");
        llvm::Value* notSeen = builder_.CreateICmpEQ(seen, builder_.getInt8(0), "cdeser.nseen");
        auto* missBB = llvm::BasicBlock::Create(llvm_, "cdeser.miss", currentFn_);
        auto* okBB   = llvm::BasicBlock::Create(llvm_, "cdeser.ok",   currentFn_);
        builder_.CreateCondBr(notSeen, missBB, okBB);
        builder_.SetInsertPoint(missBB);
        // fix #2460 / §9.8 / §28.3/§28.9/§28.13 (v0.235): this block used to
        // walk `comps`/`slots` itself and manually dispose+null every
        // non-null class-typed slot before the throw — see the errBB block
        // above (a few dozen lines up in this same function) for the full
        // account of why that classification (`ty->kind == Named &&
        // ty->declaration && Class`, with no `comp.isBackEdge` exclusion)
        // double-freed a graph-mode back-edge slot alongside the owned
        // object it borrows (confirmed heap-use-after-free, #2460). Deleted
        // for the same reason: emitThrowBuiltin's own emitUnwindDisposals()
        // call already sweeps the #2397 component-disposal scope pushed near
        // the top of this function, which classifies every component shape
        // correctly (including excluding back-edges) and is armed
        // unconditionally before this check ever runs — a second, separately
        // maintained walk here added risk without adding coverage.
        emitThrowBuiltin("SerializationException");
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateUnreachable();
        builder_.SetInsertPoint(okBB);
    }

    // fix #2378 / §9.8 / §28.3/§28.9: pop the component-disposal scope pushed
    // near the top of this function (guarding already-populated owned-class/
    // string slots against a later throw during the field loop above). Every
    // required field is now confirmed present and construction is about to
    // begin, consuming each slot's value into the constructed object (or
    // handing off to the already-correct construction-phase cleanup, #2363,
    // and emitNewPendingCheck's own partial-object disposal) — nothing in
    // this scope should be disposed on this (normal) path, so pop it WITHOUT
    // invoking its disposals (mirroring how a `using`/try body scope that
    // completes normally is handled by its own, different, exit dispatch).
    // Balances the unconditional disposeStack_.emplace_back() above (that
    // push is unconditional so the scope exists — possibly empty, for
    // isGraphMode — regardless; this pop must be unconditional too, or a
    // graph-mode class would leave disposeStack_ permanently unbalanced for
    // the rest of the enclosing function).
    disposeStack_.pop_back();

    // Allocate the class instance.
    llvm::Value* sizeofSt = builder_.CreatePtrToInt(
        builder_.CreateGEP(st, llvm::ConstantPointerNull::get(ptrTy()),
                           {builder_.getInt32(1)}),
        i64());
    llvm::Value* obj = builder_.CreateCall(callocFn(), {builder_.getInt64(1), sizeofSt});

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

    // §9.8 / #2325: the class's PRIMARY constructor (§11.1) is itself a
    // match candidate — see the parallel sema check
    // (TypeResolution_Checks.cpp checkSerializationEligibility), which
    // already validated that exactly one candidate (primary or explicit)
    // matches whenever this codegen runs. There is no ConstructorSymbol / no
    // ctorFns_ entry for the primary constructor (SymbolCollection.cpp
    // creates a ConstructorSymbol only for an explicit ast::ConstructorDecl,
    // and the primary constructor of a no-explicit-ctor class is not lowered
    // as a separate callable function at all — see lowerNew's no-explicit-
    // ctor path, LlvmBackend_Exprs2.cpp), so it cannot be "called" — instead,
    // mirror lowerNew's own construction order for that case: store each
    // deserialized component value directly into its primary-ctor-parameter
    // field slot (transfer, no retain — the deserialized value is a fresh +1
    // exactly like a literal argument to `new`, LlvmBackend_Exprs2.cpp's
    // #1605 comment), THEN run field initializers (emitInstanceInit) so that
    // any explicit field whose initializer reads a primary-ctor parameter
    // (`Id: int := id;`) — as well as a parameter captured ONLY by a
    // computed property (`property Id: int => id;`) — observes the correct
    // value, exactly as ordinary `new ClassName(...)` construction does.
    // fix #3140 (family F2 of #3152): the primary-ctor parameter list comes
    // from `sema::recordPositionalFieldSymbols` (#3141's both-sources indexed
    // view) rather than from a `dynamic_cast` to `ast::TypeDecl`, so this walk
    // no longer holds its own opinion on whether the class has one. It is the
    // last `ts->decl` read this function had.
    bool primaryCtorUsed = false;
    const std::vector<const Symbol*> primaryParams =
        sema::recordPositionalFieldSymbols(ts);
    if (ts->hasPositionalFields && primaryParams.size() == comps.size()) {
        bool allMatch = true;
        for (const Symbol* pfs : primaryParams) {
            if (compByNameLower.find(toLowerAscii(pfs->name)) ==
                    compByNameLower.end()) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) {
            for (const Symbol* pfs : primaryParams) {
                // `pfs` is the primary-ctor param's OWN FieldSymbol, whose
                // struct slot may be a different symbol than the component that
                // supplies its value (e.g. `Id: int := id;`: component "Id"
                // supplies the value stored into param `id`'s slot).
                auto fit = fieldIndex_.find(pfs);
                if (fit == fieldIndex_.end()) continue;
                auto cit = compByNameLower.find(toLowerAscii(pfs->name));
                if (cit == compByNameLower.end()) continue;
                std::size_t ci = cit->second;
                if (!comps[ci].ty) continue;
                llvm::Type* lt = lowerTypeRef(comps[ci].ty);
                if (!lt) continue;
                llvm::Value* fv;
                if (comps[ci].isSkip || !slots[ci])
                    fv = llvm::Constant::getNullValue(lt);
                else
                    fv = builder_.CreateLoad(lt, slots[ci], "cdeser.pcarg");
                builder_.CreateStore(
                    fv, builder_.CreateStructGEP(st, obj, fit->second));
            }
            primaryCtorUsed = true;
        }
    }

    // emitInstanceInit installs ONLY the vtable (LlvmBackend_Setup.cpp) — it
    // does NOT run field initializers despite the name; those are run by
    // emitOwnFieldInit, normally invoked from inside a constructor's own
    // compiled LLVM body (the explicit-ctor path below reaches it that way)
    // or, for a no-explicit-ctor `new` expression, explicitly at the call
    // site (LlvmBackend_Exprs2.cpp's `!hasAnyConstructor(t)` branch). The
    // primary-ctor path above has no ctor function to call, so — mirroring
    // that no-explicit-ctor `new` site exactly — emitOwnFieldInit must be
    // called explicitly here too, or an explicit field whose initializer
    // reads a primary-ctor parameter (`Id: int := id;`) is silently left at
    // its calloc-zeroed default (verified: this was the #2325 bug — a
    // Widget2-shaped class round-tripped Id=0 instead of the deserialized
    // value, because only emitInstanceInit ran, not emitOwnFieldInit).
    //
    // fix #2363 / §9.8 v0.234 ("each owned object is constructed by an
    // ordinary call to its constructor") + §22.9 v0.234 "Owned fields and
    // exception safety": "An exception during a field initializer (step 2)
    // triggers disposal of any earlier derived owned fields whose
    // initializers completed, in reverse declaration order, followed by the
    // base subobject's Dispose() ... The exception then propagates" and "the
    // derived instance is never observable". Since §9.8 construction is
    // specified as an ordinary constructor call, the same exception-safety
    // guarantee applies here: mirror the no-explicit-ctor `new` site's
    // throw-safety cleanup (LlvmBackend_Exprs2.cpp's `!hasAnyConstructor(t)`
    // branch) around this emitOwnFieldInit call, so a throwing own field
    // initializer disposes the already-populated primary-ctor-param fields
    // (stored above — §22.9's step-0 capture fields, "whose initialization
    // completed first") and any earlier own-field initializer's owned value,
    // plus the base subobject (a no-op today: #2325/#2364 reject base
    // storage pre-codegen, but mirrored so the cleanup stays exact if that
    // restriction is ever lifted), then reclaims the instance block, before
    // re-propagating outward — exactly as an ordinary `new` on this shape
    // would.
    {
        llvm::Value* savedThis = currentThis_;
        currentThis_ = obj;
        emitInstanceInit(ts, obj, st);
        if (primaryCtorUsed) {
            llvm::BasicBlock* savedUnwind = unwindTarget_;
            std::size_t savedDepth = unwindScopeDepth_;
            auto* savedInsert = builder_.GetInsertBlock();
            llvm::BasicBlock* enclosingUnwind = unwindBB();
            auto* cleanupBB = llvm::BasicBlock::Create(
                llvm_, "cdeser.pcfield.cleanup", currentFn_);
            builder_.SetInsertPoint(cleanupBB);
            emitOwnedFieldDisposals(ts);
            emitBaseDisposeChain(ts);
            builder_.CreateCall(freeFn(), {obj});
            builder_.CreateBr(enclosingUnwind);
            builder_.SetInsertPoint(savedInsert);
            unwindTarget_ = cleanupBB;
            unwindScopeDepth_ = disposeStack_.size();
            emitOwnFieldInit(ts, obj, st);
            unwindTarget_ = savedUnwind;
            unwindScopeDepth_ = savedDepth;
        }
        currentThis_ = savedThis;
    }

    sema::ConstructorSymbol* matchedCtor = nullptr;
    if (!primaryCtorUsed) {
        for (Symbol* m : ts->members) {
            auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
            if (!cs || cs->isStatic) continue;
            auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
            if (!cd) continue;
            // Match: param count == component count AND every param name
            // (lowered) maps to a distinct component (bijection).
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

// fix #2996: gate the built-in serializer dispatch on the SAME shape
// tryLowerSerializationCall accepts — a TypeRef that is either (a) the
// §2.15/§2.14 opaque handle (no `declaration` at all) or (b) a
// builtinClassSym (`!decl && moduleName.empty()`, the #2997 discriminator).
// A user class with the same name (spec §11.3 reserves no names) carries a
// real `declaration` and is therefore NOT a serialization call — the
// ordinary method-dispatch path must run. Without this gate the pre-check
// returned true for a user class named BinarySerializer, but the actual
// tryLowerSerializationCall body then refused to handle the call (its
// own discriminator skipped the arm), so the dispatch returned nullptr
// and the call degraded to the "return-value expression failed to lower"
// internal: note (the user method body never ran).
static bool isBuiltinSerializerTypeRef(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
    // Shape (a): opaque handle, no TypeSymbol at all.
    if (!rt->declaration) return true;
    // Shape (b): builtinClassSym — `decl` is null and `moduleName` is empty
    // (a synthetic builtin), so an IMPORTED user class (non-empty moduleName)
    // is correctly excluded. See builtinTypeName in LlvmBackend_Calls.cpp.
    return !rt->declaration->decl && rt->declaration->moduleName.empty();
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
        !recvTy->astNode->name->parts.empty() &&
        isBuiltinSerializerTypeRef(recvTy)) {
        const std::string_view rcn = recvTy->headName();
        if (rcn == "BinarySerializer" || rcn == "BinaryDeserializer") return true;
        // §2.15: JsonSerializer / JsonDeserializer instance methods.
        if (rcn == "JsonSerializer" || rcn == "JsonDeserializer") return true;
        // §2.14: ISerializer / IDeserializer methods (used in hand-written
        // Serialize/Deserialize bodies; the concrete object is always a
        // BinarySerializer* / BinaryDeserializer* at runtime).
        if (rcn == "ISerializer" || rcn == "IDeserializer") return true;
        // §4.14 / #636: OwningBuffer<T>.AsSpan() / AsReadOnlySpan() / Count /
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
                 ts->kind == ast::TypeDeclKind::Enum ||
                 ts->kind == ast::TypeDeclKind::Choice) &&  // §9.8 v0.200 #2046
                declaresConformance(ts, "ISerializable") &&
                !hasHandWrittenMethod(ts, "Serialize"))
                return true;
            // §9.8 tier 2: synthesized Serialize on a class with serialized clause.
            if (ts->kind == ast::TypeDeclKind::Class &&
                declaresConformance(ts, "ISerializable") &&
                !hasHandWrittenMethod(ts, "Serialize")) {
                // fix #3140 (family F2 of #3152): the class's §9.8 surface,
                // from the symbol.
                if (!sema::serializedClassShape(ts).empty()) return true;
            }
        }
    }

    // Synthesized static Deserialize on record/enum/class type name.
    if (mn == "Deserialize" && call.args.size() == 1) {
        const sema::Symbol* sym = ctx_.exprSymbol(*ma.target);
        if (auto* ts = dynamic_cast<const TypeSymbol*>(sym)) {
            if ((ts->kind == ast::TypeDeclKind::Record ||
                 ts->kind == ast::TypeDeclKind::Enum ||
                 ts->kind == ast::TypeDeclKind::Choice) &&  // §9.8 v0.200 #2046
                declaresConformance(ts, "IDeserializable") &&
                !hasHandWrittenMethod(ts, "Deserialize"))
                return true;
            // §9.8 tier 2: synthesized Deserialize on a class with serialized clause.
            if (ts->kind == ast::TypeDeclKind::Class &&
                declaresConformance(ts, "IDeserializable") &&
                !hasHandWrittenMethod(ts, "Deserialize")) {
                // fix #3140 (family F2 of #3152): the class's §9.8 surface,
                // from the symbol.
                if (!sema::serializedClassShape(ts).empty()) return true;
            }
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Emitter::lowerSerializerWriteMethod  fix #1397 / §2.14
//
// Forward an ISerializer write/structure method to the bz_binser_* runtime.
// Shared by the concrete BinarySerializer / JsonSerializer receivers and the
// ISerializer interface receiver: the write functions branch on the buffer's
// kind tag internally (binary vs JSON), so the same lowering serves both.
// Returns true iff `methodName` named a write/structure method (all void).
// ---------------------------------------------------------------------------
bool Emitter::lowerSerializerWriteMethod(llvm::Value* ser,
                                         std::string_view methodName,
                                         const ast::CallExpr& call) {
    auto voidTy = llvm::Type::getVoidTy(llvm_);

    if (methodName == "BeginObject" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* cnt = lowerExpr(*call.args[0].value);
        if (!cnt) return true;
        cnt = builder_.CreateSExtOrTrunc(cnt, i32(), "begin.cnt");
        auto* fn = declareC("bz_binser_begin_object", voidTy, {ptrTy(), i32()});
        builder_.CreateCall(fn, {ser, cnt});
        return true;
    }
    if (methodName == "EndObject" && call.args.empty()) {
        auto* fn = declareC("bz_binser_end_object", voidTy, {ptrTy()});
        builder_.CreateCall(fn, {ser});
        return true;
    }
    if (methodName == "WriteFieldName" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* nm = lowerExpr(*call.args[0].value);
        if (!nm) return true;
        auto* fn = declareC("bz_binser_write_field_name", voidTy, {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {ser, nm});
        return true;
    }
    if (methodName == "WriteInt" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return true;
        v = builder_.CreateSExtOrTrunc(v, i64(), "wrint.v");
        auto* fn = declareC("bz_binser_write_int", voidTy, {ptrTy(), i64()});
        builder_.CreateCall(fn, {ser, v});
        return true;
    }
    if (methodName == "WriteUInt" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return true;
        v = builder_.CreateZExtOrTrunc(v, i64(), "wruint.v");
        auto* fn = declareC("bz_binser_write_uint", voidTy, {ptrTy(), i64()});
        builder_.CreateCall(fn, {ser, v});
        return true;
    }
    if (methodName == "WriteFloat" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return true;
        if (!v->getType()->isDoubleTy())
            v = builder_.CreateFPExt(v, llvm::Type::getDoubleTy(llvm_), "wrflt.v");
        auto* fn = declareC("bz_binser_write_float", voidTy,
            {ptrTy(), llvm::Type::getDoubleTy(llvm_)});
        builder_.CreateCall(fn, {ser, v});
        // §2.15 / fix #1229: bz_binser_write_float sets bz_exc for NaN/±Infinity
        // on a JsonSerializer; propagate so the caller's try/catch can handle it.
        emitPendingCheck();
        return true;
    }
    if (methodName == "WriteBool" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return true;
        v = builder_.CreateZExtOrTrunc(v, i8(), "wrbool.v");
        auto* fn = declareC("bz_binser_write_bool", voidTy, {ptrTy(), i8()});
        builder_.CreateCall(fn, {ser, v});
        return true;
    }
    if (methodName == "WriteString" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return true;
        auto* fn = declareC("bz_binser_write_string", voidTy, {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {ser, v});
        return true;
    }
    if (methodName == "WriteChar" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return true;
        v = builder_.CreateSExtOrTrunc(v, i32(), "wrchar.v");
        auto* fn = declareC("bz_binser_write_char", voidTy, {ptrTy(), i32()});
        builder_.CreateCall(fn, {ser, v});
        return true;
    }
    if (methodName == "WriteDecimal" && call.args.size() == 1 && call.args[0].value) {
        // §2.14 / #665: ISerializer.WriteDecimal — pass the 16-byte decimal
        // struct by pointer (4×u32 LE wire format).
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return true;
        llvm::AllocaInst* decSlot = entryAlloca(decimalStructType());
        if (v->getType() == decimalStructType())
            builder_.CreateStore(v, decSlot);
        auto* fn = declareC("bz_binser_write_decimal", voidTy,
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {ser, decSlot});
        return true;
    }
    if (methodName == "WriteNull" && call.args.empty()) {
        auto* fn = declareC("bz_binser_write_null", voidTy, {ptrTy()});
        builder_.CreateCall(fn, {ser});
        return true;
    }
    if (methodName == "BeginArray" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* cnt = lowerExpr(*call.args[0].value);
        if (!cnt) return true;
        cnt = builder_.CreateSExtOrTrunc(cnt, i32(), "bgarr.cnt");
        auto* fn = declareC("bz_binser_begin_array", voidTy, {ptrTy(), i32()});
        builder_.CreateCall(fn, {ser, cnt});
        return true;
    }
    if (methodName == "EndArray" && call.args.empty()) {
        auto* fn = declareC("bz_binser_end_array", voidTy, {ptrTy()});
        builder_.CreateCall(fn, {ser});
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Emitter::lowerDeserializerReadMethod  fix #1397 / §2.14
//
// Forward an IDeserializer read/structure method to the bz_binder_* runtime.
// Shared by the concrete BinaryDeserializer / JsonDeserializer receivers and
// the IDeserializer interface receiver (the read functions branch on the
// buffer's kind tag internally).  Sets `handled` iff `methodName` named a
// read/structure method; returns the lowered value (nullptr for the void
// methods EndObject/EndArray/SkipValue).  A read on a JSON deserializer may
// raise SerializationException (type mismatch / truncation) — emitPendingCheck
// propagates it to the caller's try/catch.
// ---------------------------------------------------------------------------
llvm::Value* Emitter::lowerDeserializerReadMethod(llvm::Value* der,
                                                  std::string_view methodName,
                                                  const ast::CallExpr& call,
                                                  bool& handled) {
    handled = true;
    auto voidTy = llvm::Type::getVoidTy(llvm_);
    auto* dblTy = llvm::Type::getDoubleTy(llvm_);

    // Value reads (§2.14 IDeserializer): return the stdlib-declared width.
    if (methodName == "ReadInt" && call.args.empty()) {
        auto* fn = declareC("bz_binder_read_int", i64(), {ptrTy()});
        llvm::Value* v = builder_.CreateCall(fn, {der}, "der.int");
        emitPendingCheck();
        return v;  // long
    }
    if (methodName == "ReadUInt" && call.args.empty()) {
        auto* fn = declareC("bz_binder_read_uint", i64(), {ptrTy()});
        llvm::Value* v = builder_.CreateCall(fn, {der}, "der.uint");
        emitPendingCheck();
        return v;  // ulong
    }
    if (methodName == "ReadFloat" && call.args.empty()) {
        auto* fn = declareC("bz_binder_read_float", dblTy, {ptrTy()});
        llvm::Value* v = builder_.CreateCall(fn, {der}, "der.float");
        emitPendingCheck();
        return v;  // double
    }
    if (methodName == "ReadDecimal" && call.args.empty()) {
        // #665: bz_binder_read_decimal returns a ptr to a 16-byte {i32×4} struct
        // on the runtime side; load it to a value (null → default(decimal)=0).
        auto* fn = declareC("bz_binder_read_decimal", ptrTy(), {ptrTy()});
        llvm::Value* decPtr = builder_.CreateCall(fn, {der}, "der.decptr");
        emitPendingCheck();
        llvm::AllocaInst* slot = entryAlloca(decimalStructType());
        builder_.CreateStore(
            llvm::Constant::getNullValue(decimalStructType()), slot);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            decPtr, llvm::ConstantPointerNull::get(ptrTy()), "der.dec.null");
        auto* loadBB = llvm::BasicBlock::Create(llvm_, "der.dec.load", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "der.dec.end",  currentFn_);
        builder_.CreateCondBr(isNull, endBB, loadBB);
        builder_.SetInsertPoint(loadBB);
        builder_.CreateStore(
            builder_.CreateLoad(decimalStructType(), decPtr, "der.decv"), slot);
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        return builder_.CreateLoad(decimalStructType(), slot, "der.dec");
    }
    if (methodName == "ReadBool" && call.args.empty()) {
        auto* fn = declareC("bz_binder_read_bool", i8(), {ptrTy()});
        llvm::Value* b8 = builder_.CreateCall(fn, {der}, "der.bool8");
        emitPendingCheck();
        return builder_.CreateTrunc(b8, i1(), "der.bool");
    }
    if (methodName == "ReadString" && call.args.empty()) {
        // Returns a newly-allocated string (rc=1); the caller owns it.
        auto* fn = declareC("bz_binder_read_string", ptrTy(), {ptrTy()});
        llvm::Value* v = builder_.CreateCall(fn, {der}, "der.str");
        emitPendingCheck();
        return v;
    }
    if (methodName == "ReadChar" && call.args.empty()) {
        auto* fn = declareC("bz_binder_read_char", i32(), {ptrTy()});
        llvm::Value* v = builder_.CreateCall(fn, {der}, "der.char");
        emitPendingCheck();
        return v;
    }
    if (methodName == "TryReadNull" && call.args.empty()) {
        auto* fn = declareC("bz_binder_try_read_null", i8(), {ptrTy()});
        llvm::Value* b8 = builder_.CreateCall(fn, {der}, "der.trynull8");
        return builder_.CreateTrunc(b8, i1(), "der.trynull");
    }
    if (methodName == "BeginObject" && call.args.empty()) {
        auto* fn = declareC("bz_binder_begin_object", i32(), {ptrTy()});
        llvm::Value* v = builder_.CreateCall(fn, {der}, "der.beginobj");
        emitPendingCheck();
        return v;  // int field count (or -1 for JSON)
    }
    if (methodName == "ReadFieldName" && call.args.empty()) {
        auto* fn = declareC("bz_binder_read_field_name", ptrTy(), {ptrTy()});
        llvm::Value* v = builder_.CreateCall(fn, {der}, "der.fieldname");
        emitPendingCheck();
        return v;
    }
    if (methodName == "SkipValue" && call.args.empty()) {
        auto* fn = declareC("bz_binder_skip_value", voidTy, {ptrTy()});
        builder_.CreateCall(fn, {der});
        emitPendingCheck();
        return nullptr;  // void
    }
    if (methodName == "EndObject" && call.args.empty()) {
        auto* fn = declareC("bz_binder_end_object", voidTy, {ptrTy()});
        builder_.CreateCall(fn, {der});
        emitPendingCheck();
        return nullptr;  // void
    }
    if (methodName == "BeginArray" && call.args.empty()) {
        auto* fn = declareC("bz_binder_begin_array", i32(), {ptrTy()});
        llvm::Value* v = builder_.CreateCall(fn, {der}, "der.beginarr");
        emitPendingCheck();
        return v;  // int element count (or -1 for JSON)
    }
    if (methodName == "HasMoreElements" && call.args.empty()) {
        auto* fn = declareC("bz_binder_has_more_elements", i8(), {ptrTy()});
        llvm::Value* b8 = builder_.CreateCall(fn, {der}, "der.hasmore8");
        return builder_.CreateTrunc(b8, i1(), "der.hasmore");
    }
    if (methodName == "EndArray" && call.args.empty()) {
        auto* fn = declareC("bz_binder_end_array", voidTy, {ptrTy()});
        builder_.CreateCall(fn, {der});
        emitPendingCheck();
        return nullptr;  // void
    }

    handled = false;
    return nullptr;
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
        const std::string_view rcn = recvTy->headName();

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
            // fix #1397: the ISerializer write surface on the concrete class —
            // previously unwired, so `ser.WriteInt(x)` was a silent no-op and
            // ToBytes() then returned an empty buffer.
            if (lowerSerializerWriteMethod(ser, methodName, call)) return nullptr;
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
            // fix #1397: the IDeserializer read surface on the concrete class.
            bool handled = false;
            llvm::Value* v = lowerDeserializerReadMethod(der, methodName, call, handled);
            if (handled) return v;
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
            // fix #1397: the ISerializer write surface on the concrete class
            // (write functions branch on kind → JSON here).
            if (lowerSerializerWriteMethod(ser, methodName, call)) return nullptr;
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
            // fix #1397: the IDeserializer read surface on the concrete class.
            bool handled = false;
            llvm::Value* v = lowerDeserializerReadMethod(der, methodName, call, handled);
            if (handled) return v;
            return nullptr;
        }

        // §2.14: ISerializer — forwarded to bz_binser_* functions.
        // The concrete object is always a BinarySerializer* / JsonSerializer*
        // at runtime; the shared helper handles the full write surface.
        if (rcn == "ISerializer") {
            llvm::Value* ser = lowerExpr(*ma->target);
            if (!ser) return nullptr;
            lowerSerializerWriteMethod(ser, methodName, call);
            return nullptr;
        }

        // §2.14: IDeserializer — forwarded to bz_binder_* functions.  fix #1397:
        // wire the read surface so hand-written Deserialize bodies that call
        // `d.ReadInt()`/`d.BeginObject()`/… (stdlib §2.14 example) move data.
        if (rcn == "IDeserializer") {
            llvm::Value* der = lowerExpr(*ma->target);
            if (!der) return nullptr;
            bool handled = false;
            llvm::Value* v = lowerDeserializerReadMethod(der, methodName, call, handled);
            if (handled) return v;
            return nullptr;
        }

        // §4.14 / #636: OwningBuffer<T>.AsSpan() / AsReadOnlySpan() / Count /
        // Length are no longer handled here — they are delegated to:
        //   - AsSpan / AsReadOnlySpan: tryLowerArraySpanCall (same lowering
        //     path as Array<T>; handles all element types with correct
        //     element-width and bounds checks)
        //   - Count / Length: tryLowerCollectionInstanceCall (bz_array_len)
        // Since isSerializationCall no longer flags these calls, this branch
        // is never reached for OwningBuffer/Array AsSpan/Count/Length.
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
            bool isChoice = ts->kind == ast::TypeDeclKind::Choice;
            if ((isRecord || isEnum || isClass || isChoice) &&
                declaresConformance(ts, "ISerializable") &&
                !hasHandWrittenMethod(ts, "Serialize")) {

                // fix #3140 (family F2 of #3152): the class's §9.8 surface,
                // from the symbol.
                if (isClass && sema::serializedClassShape(ts).empty())
                    return nullptr;

                llvm::Value* serArg = lowerExpr(*call.args[0].value);
                if (!serArg) return nullptr;
                llvm::Value* recv = lowerExpr(*ma->target);
                if (!recv) return nullptr;

                if (isRecord) {
                    llvm::StructType* st = recordStruct(ts);
                    if (recv->getType()->isPointerTy())
                        recv = builder_.CreateLoad(st, recv);
                    emitRecordSerialize(ts, recv, serArg);
                } else if (isChoice) {
                    // §9.8 choice serialization (v0.200, #2046): the `$case`-
                    // discriminated object form.  `recv` is a value-type choice
                    // struct; load it if it came through as a slot pointer.
                    llvm::StructType* st = choiceStruct(ts);
                    if (recv->getType()->isPointerTy())
                        recv = builder_.CreateLoad(st, recv);
                    emitChoiceSerialize(ts, recv, serArg);
                } else if (isEnum) {
                    // §10.3 (#740): hand emitEnumSerialize the value at the
                    // enum's underlying width (it now compares at that width).
                    llvm::Type* uTy = enumLlvmType(ts);
                    if (recv->getType() != uTy)
                        recv = builder_.CreateSExtOrTrunc(recv, uTy, "enum.v");
                    emitEnumSerialize(ts, recv, serArg);
                } else {
                    // Class: recv is a ptr to the class instance.
                    // §9.8 tier 4: polymorphic base → poly dispatch.
                    // fix #3140 (family F2 of #3152): the registry, from the
                    // symbol.
                    if (!sema::serializedPolyCasesOf(ts).empty())
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
                    ts->kind == ast::TypeDeclKind::Enum ||
                    ts->kind == ast::TypeDeclKind::Choice) {
                    llvm::Value* derArg = lowerExpr(*call.args[0].value);
                    if (!derArg) return nullptr;
                    if (ts->kind == ast::TypeDeclKind::Record)
                        return emitRecordDeserialize(ts, derArg);
                    else if (ts->kind == ast::TypeDeclKind::Choice)
                        // §9.8 choice serialization (v0.200, #2046).
                        return emitChoiceDeserialize(ts, derArg);
                    else
                        return emitEnumDeserialize(ts, derArg);
                }
                if (ts->kind == ast::TypeDeclKind::Class) {
                    // fix #3140 (family F2 of #3152): the class's §9.8 surface
                    // and registry, from the symbol.
                    if (sema::serializedClassShape(ts).empty()) return nullptr;
                    llvm::Value* derArg = lowerExpr(*call.args[0].value);
                    if (!derArg) return nullptr;
                    // §9.8 tier 4: polymorphic base → poly dispatch.
                    if (!sema::serializedPolyCasesOf(ts).empty())
                        return emitPolyClassDeserialize(ts, derArg);
                    return emitClassDeserialize(ts, derArg);
                }
            }
        }
    }

    return nullptr;
}

} // namespace bezel::codegen
