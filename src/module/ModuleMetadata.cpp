// Module public-interface metadata: bitstream serialize/deserialize +
// semantic hash (§2.5 / §2.6, issue #328 increment 1).
//
// Wire format (issue #328: LLVM bitstream, schema-version + semantic-hash
// header, read-older / skip-unknown semantics):
//
//   The whole blob is one bitstream MODULE_BLOCK subblock. Its first record is
//   HEADER [magic, schemaVersion, hashLo, hashHi]; a reader validates magic +
//   version from it before trusting the body. The body is a sequence of
//   records and nested subblocks. Strings are length-prefixed records of one
//   byte per value. Every composite (a type, a member, a function, a constant,
//   a generic param, a parameter, a sub-type) is its own nested subblock so a
//   reader that meets an UNKNOWN subblock id (a future schema's addition) can
//   SkipBlock() it, and an unknown record code can skipRecord() it — the
//   forward-compatible Java .class model from issue #328.
//
//   Numeric record/subblock ids below are part of the wire format; appending
//   new ones is additive (old readers skip them), so it does not bump
//   kSchemaVersion. Renumbering or repurposing an existing id WOULD.

#include "module/ModuleMetadata.hpp"

#include <cstring>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Bitstream/BitstreamWriter.h"

namespace bezel::module {

namespace {

// --- Block ids (>= FIRST_APPLICATION_BLOCKID = 8) ---------------------------
enum BlockID : unsigned {
    MODULE_BLOCK = 8,   // the whole blob
    TYPE_BLOCK = 9,     // one MetaTypeDecl
    MEMBER_BLOCK = 10,  // one MetaMember
    FUNC_BLOCK = 11,    // one MetaFunction
    CONST_BLOCK = 12,   // one MetaConstant
    GENERIC_BLOCK = 13, // one MetaGenericParam
    PARAM_BLOCK = 14,   // one MetaParam
    METATYPE_BLOCK = 15, // one MetaType (a signature type, possibly nested)
    CONTRACT_BLOCK = 16, // one MetaContractRecord (§22.16 / #2054)
    EXTENSION_BLOCK = 17, // one MetaExtension (§26 / #735)
    // §9.8 class shape + polymorphic registry (#3179, family F10 of umbrella
    // #3152; block ids in the additive 20.. range reserved by #3144).
    POLY_CASE_BLOCK = 20,  // one MetaPolyCase
    // §35.1 (#3144): one MetaChoiceCase / one MetaChoicePayloadField. These
    // ride as TRAILING subblocks of TYPE_BLOCK with no count scalar — see the
    // ⚠ block in emitTypeDecl for why a 9th positional scalar was not an
    // option — so they are self-describing and every reader this format has
    // ever had SkipBlock()s them.
    CHOICE_CASE_BLOCK = 18,    // one MetaChoiceCase (§35.1 / #3144)
    CHOICE_PAYLOAD_BLOCK = 19, // one MetaChoicePayloadField (§35.1 / #3144)
    // 20.. reserved for future additive subblocks (a reader skips unknown ids)
    UNKNOWN_FUTURE_BLOCK = 99, // test-only: a subblock id this reader skips
};

// --- Record codes (>= FIRST_APPLICATION_ABBREV = 4; we never abbreviate) ----
// Generic scalar/string records reused across blocks.
enum RecordCode : unsigned {
    // MODULE_BLOCK
    REC_HEADER = 4,      // [magic, schemaVersion, hashLo, hashHi]
    REC_MODULE_NAME = 5, // string
    REC_EXPORT = 6,      // string (one per export)
    // shared
    REC_STRING = 7,      // a string payload record (bytes as values)
    REC_SCALARS = 8,     // a packed list of small scalars (kind/flags/etc.)
    // §12 / §31.4 (#2950): TYPE_BLOCK — the built-in exception base name, as a
    // string payload under its OWN code. A distinct code (rather than a second
    // trailing REC_STRING) is what keeps it independent of #662c's trailing
    // `dtorBodyText` string: neither can be read as the other, in either order,
    // and either may be absent. Unknown codes are skipRecord()'d, so a reader
    // predating this field is unaffected — no schema bump (see kSchemaVersion).
    REC_EXC_BASE = 9,    // string: §31.4 built-in exception base of a class
    // fix #3140 (§9.8, family F2 of umbrella #3152): MEMBER_BLOCK — a storage
    // member's `serialized` clause, as `[skip, optional, <wire-name bytes>…]`
    // under its OWN code. A distinct code rather than an 18th positional scalar
    // or a third trailing REC_STRING, for the reason emitTypeDecl spells out
    // for #3139's conditional 8th scalar: a positional entry can only be
    // unconditional (perturbing every module's bytes and header hash) or
    // conditional (making the NEXT author's addition a silent misparse), while
    // a coded record is self-describing, absent when unused, and skipRecord()'d
    // by every reader this format has had.
    REC_SER_CLAUSE = 10,
    // fix #3191 (§3.6 / §28.9 / §21.9 / §2.8.2, family M2 of umbrella #3152):
    // TYPE_BLOCK — the §3.6 trivial-dispose verdict, INVERTED, as a single
    // scalar under its OWN code. Emitted only when TRUE, so an artifact whose
    // types are all trivial-dispose is byte-identical to before. A distinct code
    // for the reason the ⚠ block above `emitTypeDecl`'s 8th scalar states: a 9th
    // positional scalar would be read as #3139's `nBuiltinIfaces`, a silent
    // misparse. Absent (an OLD artifact) reads as false — pre-fix behaviour.
    REC_DISPOSE_VERDICT = 11,
    // fix #3202 (§22.10 "primary constructors" / §22.5 "Optional parameters
    // and named arguments" / §2.8.3 rule 4, v0.254): MEMBER_BLOCK — the
    // positional parameter's `:= Expr` default text, as a string payload
    // under its OWN code. Emitted only when the parameter carries a default
    // that `renderConstExpr` could render, so a module whose primary
    // constructor parameters have no defaults is byte-identical to before.
    // A distinct code (rather than a third trailing REC_STRING after
    // `caps[1]`'s `fieldInitText`) for the same reason REC_SER_CLAUSE /
    // REC_DISPOSE_VERDICT above are coded: adding a third positional REC_STRING
    // makes the reader's `capIdx` ordering load-bearing for any future
    // addition — a coded record is self-describing, and `skipRecord()`'d by
    // every reader this format has had. Absent (an OLD artifact) reads as
    // "no default" — the pre-fix behaviour a stale dependency already
    // exhibits — so a recompile is required to gain the fix, with no
    // silent wrong-value in the dependency-direction.
    REC_POSPARAM_DEFAULT = 12,
    // §9.8 class shape + polymorphic registry (#3179, family F10 of umbrella
    // #3152; additive within v7): TYPE_BLOCK — the §9.8 class wire SHAPE,
    // carried as the bytes of one of the A.3 grammar's three identifiers
    // ("tree" / "graph" / "cyclic") under its OWN code. Emitted only when
    // non-empty — the same #2950/#3139/#3144/#3191 conditional-emission rule.
    // A reader predating this field `skipRecord()`s the unknown code, leaving
    // the shape empty (the pre-fix reading, which is exactly what the §9.8
    // surface admits for a non-serializable class). The serializedPolyCases
    // ride as trailing POLY_CASE_BLOCK subblocks under their own self-
    // describing block id (same as #3144's CHOICE_CASE_BLOCK); they are read
    // separately, in `readTypeDeclBody`, and do not need their own record
    // code here.
    REC_SER_TYPE_SHAPE = 13,
    // 14.. additive; unknown codes are skipRecord()'d by the reader
    REC_UNKNOWN_FUTURE = 90, // test-only: a record code this reader skips
};

// FNV-1a 64-bit over a byte. Used to fold structured content into the
// content-derived semantic hash (issue #328). The constants are the standard
// FNV-1a 64-bit offset basis and prime.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hashByte(std::uint64_t& h, std::uint8_t b) {
    h ^= b;
    h *= kFnvPrime;
}
void hashU64(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) hashByte(h, static_cast<std::uint8_t>(v >> (i * 8)));
}
void hashStr(std::uint64_t& h, const std::string& s) {
    hashU64(h, s.size());
    for (char c : s) hashByte(h, static_cast<std::uint8_t>(c));
}
void hashBool(std::uint64_t& h, bool b) { hashByte(h, b ? 1 : 0); }

void hashType(std::uint64_t& h, const MetaType& t) {
    hashByte(h, static_cast<std::uint8_t>(t.kind));
    hashBool(h, t.nullable);
    hashByte(h, static_cast<std::uint8_t>(t.primitive));
    hashStr(h, t.namedName);
    hashU64(h, t.genericArgs.size());
    for (const MetaType& g : t.genericArgs) hashType(h, g);
    hashStr(h, t.arraySize);
    hashBool(h, t.arrayOwned);
    hashU64(h, t.children.size());
    for (const MetaType& c : t.children) hashType(h, c);
    hashU64(h, t.funcParamMods.size());
    for (const std::string& m : t.funcParamMods) hashStr(h, m);
    hashU64(h, t.funcParamNames.size());
    for (const std::string& n : t.funcParamNames) hashStr(h, n);
    hashBool(h, t.funcReturnOwned);
}

void hashGeneric(std::uint64_t& h, const MetaGenericParam& g) {
    hashStr(h, g.name);
    hashStr(h, g.variance);
    hashU64(h, g.constraintTypes.size());
    for (const MetaType& c : g.constraintTypes) hashType(h, c);
    hashU64(h, g.specialConstraints.size());
    for (const std::string& s : g.specialConstraints) hashStr(h, s);
}

void hashParam(std::uint64_t& h, const MetaParam& p) {
    hashStr(h, p.name);
    hashStr(h, p.modifier);
    hashType(h, p.type);
    hashBool(h, p.hasDefault);
    hashStr(h, p.defaultText);
    // §22.4 / §12.1 (#2392): part of the parameter's signature identity —
    // §2.8.3 rule 3 requires every embedded description of a type/member to
    // agree, and a differing postcondition is exactly the kind of signature
    // disagreement that rule flags as staleness.
    hashByte(h, p.outNullWhen);
}

void hashContract(std::uint64_t& h, const MetaContractRecord& c) {
    hashByte(h, static_cast<std::uint8_t>(c.kind));
    hashBool(h, c.isStatic);
    hashStr(h, c.canonicalText);
    hashByte(h, static_cast<std::uint8_t>(c.tier));
}

void hashTypeDecl(std::uint64_t& h, const MetaTypeDecl& ty);  // fwd

void hashMember(std::uint64_t& h, const MetaMember& m) {
    hashByte(h, static_cast<std::uint8_t>(m.kind));
    hashStr(h, m.name);
    hashByte(h, static_cast<std::uint8_t>(m.visibility));
    hashU64(h, m.modifiers.size());
    for (const std::string& s : m.modifiers) hashStr(h, s);
    hashBool(h, m.isPure);
    // fix #2400 (§23/§2.8.2/§2.8.3 rule 3): hasSetter is part of a Property's
    // signature identity — a library changing a property from getter-only to
    // read-write (or vice versa) changes the cross-module contract exactly
    // the way #2392's outNullWhen does for a postcondition, so it must fold
    // into the semantic hash or a consumer's staleness check cannot see the
    // change (§2.8.3 rule 3: staleness is an error, not a negotiation).
    hashBool(h, m.hasSetter);
    // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4): the setter's
    // EFFECTIVE visibility is also part of the Property's signature
    // identity — a library tightening `set` to `private set` (or
    // loosening it) changes the cross-module contract, so it must fold
    // into the semantic hash the same way hasSetter does above.
    hashByte(h, static_cast<std::uint8_t>(m.setterVisibility));
    // fix #2478 (§9.2/§22.8): isAutoProperty is part of a Property's
    // signature identity for LAYOUT purposes — a library changing a
    // property from auto-storage form to computed form (or vice versa)
    // changes the cross-module storage contract, so it must fold into the
    // semantic hash the same way hasSetter/setterVisibility do above.
    hashBool(h, m.isAutoProperty);
    // fix #2461 (§22.3/§23): hasGetter is part of a Property's signature
    // identity — a library changing a property from write-only to read-write
    // (or vice versa) changes whether a cross-module `ref` argument is
    // legal (§22.3: ref calls the getter for the copy-in), so it must fold
    // into the semantic hash the same way hasSetter does above, or a
    // consumer's staleness check cannot see the change (§2.8.3 rule 3).
    hashBool(h, m.hasGetter);
    // fix #2421 (§24/§2.8.2): an Event's static/async classification is
    // part of its signature identity — a library flipping either bit changes
    // the event's struct slot presence (static vs. instance) or itable slot
    // presence (sync vs. async), reshaping the cross-module layout contract,
    // so it must fold into the semantic hash the same way hasSetter/
    // isAutoProperty do above (§2.8.3 rule 3 staleness detection).
    hashBool(h, m.isEventStatic);
    hashBool(h, m.isEventAsync);
    // fix #2516 (§9.2/§2.8.3 rule 3): isPositional is part of a Field's
    // signature identity — a library changing a field from positional to
    // body (or vice versa) changes the cross-module contract (positional
    // fields are §9.2 auto-hashable; body fields are not), so it
    // must fold into the semantic hash the same way hasSetter/
    // isAutoProperty/hasGetter/isEventStatic/isEventAsync do above, or a
    // consumer's staleness check cannot see the change (§2.8.3 rule 3).
    hashBool(h, m.isPositional);
    // fix #3221 (§22.10/§2.8.3 rule 3): posParamBodyRef is part of a §22.10
    // primary-ctor parameter Field's signature identity — a library changing
    // a captured parameter to non-captured (or vice versa) changes the
    // cross-module layout contract (a captured parameter is a private
    // instance field of storage; a non-captured parameter is just a
    // constructor argument). It must fold into the semantic hash the same
    // way isPositional does above or a consumer's staleness check cannot
    // see the change (§2.8.3 rule 3).
    hashBool(h, m.posParamBodyRef);
    hashU64(h, m.contracts.size());
    for (const MetaContractRecord& c : m.contracts) hashContract(h, c);
    hashType(h, m.type);
    hashU64(h, m.genericParams.size());
    for (const MetaGenericParam& g : m.genericParams) hashGeneric(h, g);
    hashU64(h, m.params.size());
    for (const MetaParam& p : m.params) hashParam(h, p);
    hashBool(h, m.isOperator);
    hashBool(h, m.returnOwned);
    hashBool(h, m.hasEnumValue);
    hashU64(h, static_cast<std::uint64_t>(m.enumValue));
    // fix #3140 (§9.8 / §2.8.3 rule 3): an enum constant's serialized wire name
    // is part of its cross-module contract — renaming it changes the bytes
    // every consumer reads and writes — so it must fold into the semantic hash
    // or a consumer's staleness check cannot see the change.
    //
    // GUARDED to match the EMISSION condition at :648. Unconditionally, `hashStr`
    // hashes the length first, so an empty wire name still perturbed the hash of
    // every member of every type — measured by review: the `.o` of a module with
    // no enum at all changed by 46 bytes. Emission and hashing must agree, or the
    // header hash stops being a function of what was actually written.
    if (m.kind == MetaMemberKind::EnumConstant && !m.enumWireName.empty())
        hashStr(h, m.enumWireName);
    // fix #3140 (§9.8 / §2.8.3 rule 3, family F2): a storage member's
    // `serialized` clause is part of its cross-module contract — `skip` changes
    // the wire FIELD COUNT and a rename changes the KEY every consumer reads
    // and writes — so it must fold into the semantic hash or a consumer's
    // staleness check cannot see the change.
    //
    // ⚠ GUARDED BY THE SAME PREDICATE AS THE EMISSION (emitMember below), and
    // that is not cosmetic: an UNCONDITIONAL fold hashes a length byte even for
    // an empty string, which moves the VBR-encoded header hash and so shifts
    // every byte after it for modules that use no `serialized` clause at all.
    // That exact defect had to be corrected in this family's own F4 slice
    // (c97a49f) after review measured it.
    if (m.serializedSkip || m.serializedOptional ||
        !m.serializedWireName.empty()) {
        hashBool(h, m.serializedSkip);
        hashBool(h, m.serializedOptional);
        hashStr(h, m.serializedWireName);
    }
    // fix #3202 (§22.10 / §22.5 / §2.8.3 rule 3, v0.254): a positional
    // primary-ctor parameter's default-value text is part of the cross-module
    // contract — a library adding or removing (or changing) a `:= Expr`
    // default changes the value an under-supplied `new` constructs, so the
    // importer must see the change. GUARDED to match the EMISSION condition
    // at emitMember above: an unconditional fold would hash a length byte for
    // the empty string too, perturbing the semantic hash of every type whose
    // primary constructor carries no default. c97a49f had to correct exactly
    // this defect on the sibling REC_SER_CLAUSE half of this family.
    if (!m.posParamDefaultText.empty())
        hashStr(h, m.posParamDefaultText);
    hashStr(h, m.valueText);  // §22.12 const value (issue #328 FINDING [3] BUG A)
    // §14 / #662c (schema v5): ctor body source text for generic classes.
    hashStr(h, m.ctorBodyText);
    // §14.1 / #783 (schema v5 additive): method body source text for generic
    // classes — part of the cross-module interface (a body change must mark
    // dependents stale, since the importer monomorphizes it per instantiation).
    hashStr(h, m.methodBodyText);
    // §2.8.2 / #2351: static field initializer source text for generic
    // classes — same staleness reasoning as ctorBodyText/methodBodyText.
    hashStr(h, m.fieldInitText);
    // §14.1 / §23 / §2.8.2 / §2.8.3 rule 4 (#2893): property/indexer body
    // source text for generic classes — same staleness reasoning as
    // ctorBodyText/methodBodyText (a body change must mark dependents
    // stale, since the importer monomorphizes it per instantiation; the
    // #2884 commit message documents the silent wrong-value measurement
    // that would result if the importer cached a stale body).
    hashStr(h, m.propertyBodyText);
    // §11.3 (issue #502): hash nested type when present.
    hashBool(h, m.nestedType != nullptr);
    if (m.nestedType) hashTypeDecl(h, *m.nestedType);
}

void hashTypeDecl(std::uint64_t& h, const MetaTypeDecl& ty) {
    hashStr(h, ty.name);
    hashByte(h, static_cast<std::uint8_t>(ty.kind));
    hashByte(h, static_cast<std::uint8_t>(ty.visibility));
    hashU64(h, ty.modifiers.size());
    for (const std::string& s : ty.modifiers) hashStr(h, s);
    hashU64(h, ty.genericParams.size());
    for (const MetaGenericParam& g : ty.genericParams) hashGeneric(h, g);
    hashU64(h, ty.baseTypes.size());
    for (const MetaType& b : ty.baseTypes) hashType(h, b);
    hashU64(h, ty.members.size());
    for (const MetaMember& m : ty.members) hashMember(h, m);
    hashU64(h, ty.invariants.size());
    for (const MetaContractRecord& c : ty.invariants) hashContract(h, c);
    // §9.2 (issue #501): include the positional-record flag.
    hashBool(h, ty.hasPositionalFields);
    // §14 / #662c (schema v5): destructor body source text for generic classes.
    hashStr(h, ty.dtorBodyText);
    // §12 / §31.4 (#2950): the built-in exception base. It decides the class's
    // LAYOUT on both sides of the boundary, so it is public-interface content
    // that staleness detection must track — a dependency that gains or loses
    // `: Exception` must invalidate every dependent.
    hashStr(h, ty.builtinExceptionBase);
    // §9.8 / §18.5 / §21.4 (#3139): the built-in-interface conformance set is
    // public-interface content — a dependency that gains or loses
    // `: ISerializable` changes what its consumers may call on the type — so it
    // must fold into the semantic hash or staleness detection cannot see the
    // change (§2.8.3 rule 3: staleness is an error, not a negotiation).
    // GUARDED ON EXACTLY THE EMISSION'S CONDITION (emitTypeDecl above). The
    // hash is a function of what was WRITTEN; folding a zero count for a field
    // the writer omitted makes it a function of the compiler version instead,
    // and — because the hash is VBR-encoded into the header — moves every byte
    // of every artifact for a change that wrote nothing. Same rule, same
    // shape, as #3140's `enumWireName` fold.
    //
    // The encoding stays unambiguous: the count is folded only when it is
    // NON-ZERO, so "tail absent" and "tail present" cannot produce the same
    // stream, and this is the last thing folded for a type.
    if (!ty.builtinInterfaceBases.empty()) {
        hashU64(h, ty.builtinInterfaceBases.size());
        for (const MetaType& b : ty.builtinInterfaceBases) hashType(h, b);
    }
    // §35.1 / §35.4 (#3144): the case list is the choice's LAYOUT and its
    // classification input — adding, removing, renaming or re-typing a case or
    // a payload field changes what every consumer lays out and disposes — so it
    // is public-interface content staleness detection must track (§2.8.3 rule
    // 3). GUARDED ON EXACTLY THE EMISSION'S CONDITION (emitTypeDecl above), the
    // same rule #3139 and #3140 state: the hash is a function of what was
    // WRITTEN, and folding a zero count for a field the writer omitted moves
    // the VBR-encoded header hash — and with it every byte after it — for a
    // change that wrote nothing. The count is folded only when NON-ZERO, so
    // "tail absent" and "tail present" cannot produce the same stream.
    if (!ty.choiceCases.empty()) {
        hashU64(h, ty.choiceCases.size());
        for (const MetaChoiceCase& cc : ty.choiceCases) {
            hashStr(h, cc.name);
            hashBool(h, cc.hasSerialized);
            hashStr(h, cc.serializedName);
            hashU64(h, cc.payload.size());
            for (const MetaChoicePayloadField& pf : cc.payload) {
                hashStr(h, pf.name);
                hashStr(h, pf.modifier);
                hashBool(h, pf.hasSerialized);
                hashBool(h, pf.serializedSkip);
                hashBool(h, pf.serializedOptional);
                hashStr(h, pf.serializedName);
                hashType(h, pf.type);
            }
        }
    }
    // §3.6 / §28.9 / §2.8.3 rule 3 (#3191): the trivial-dispose verdict is
    // public-interface content staleness detection must track. §3.6 rule 3 says
    // so in as many words — "adding a destructor flips trivial-dispose … is a
    // breaking change for clients relying on it, exactly as a signature change
    // is" — and the flip changes whether a client's `new Dep();` statement is a
    // §21.9 hard error, so a dependent compiled against the old verdict is
    // stale. GUARDED ON EXACTLY THE EMISSION'S CONDITION (emitTypeDecl above):
    // folding a `false` the writer never wrote would move the VBR-encoded header
    // hash of every artifact for a change that wrote nothing.
    if (ty.nonTrivialDispose) hashBool(h, true);
    // §9.8 / §2.8.3 rule 3 (#3179): the class's serialized SHAPE is part of
    // the public contract — adding or removing `: ISerializable` (which flips
    // the shape from "" to "tree" / "graph" / "cyclic") is a breaking change
    // for any client whose `is` test, `catch` clause, or §19 pattern over a
    // value of the type's serialization surface depended on it being open or
    // closed. GUARDED ON EXACTLY THE EMISSION'S CONDITION (the same rule
    // #2950 / #3139 / #3144 / #3191 state): a fold the writer did not write
    // moves the VBR-encoded header hash of every artifact for a change that
    // wrote nothing. The "absent" and "tree / graph / cyclic" reads cannot
    // hash identically under this guard, since the guard itself forces the
    // "absent" record.
    if (!ty.serializedShape.empty()) hashStr(h, ty.serializedShape);
    // §9.8 tier 4 (#3179): the polymorphic registry entries are part of the
    // public contract — same staleness reasoning as the shape. "absent" and
    // "empty" collapse to the same read for the same reason (the slot is
    // empty), but a non-empty registry is its own self-describing content.
    if (!ty.serializedPolyCases.empty()) {
        hashU64(h, ty.serializedPolyCases.size());
        for (const MetaTypeDecl::MetaPolyCase& pc : ty.serializedPolyCases) {
            hashStr(h, pc.discriminator);
            hashType(h, pc.type);
        }
    }
}

} // namespace

// --- Semantic hash ----------------------------------------------------------

std::uint64_t semanticHash(const ModuleMetadata& md) {
    std::uint64_t h = kFnvOffset;
    // Fold a schema tag so two schema versions of the same content differ.
    hashU64(h, kSchemaVersion);
    hashStr(h, md.moduleName);
    hashU64(h, md.exports.size());
    for (const std::string& e : md.exports) hashStr(h, e);
    hashU64(h, md.types.size());
    for (const MetaTypeDecl& ty : md.types) {
        hashTypeDecl(h, ty);
    }
    hashU64(h, md.functions.size());
    for (const MetaFunction& fn : md.functions) {
        hashStr(h, fn.name);
        hashByte(h, static_cast<std::uint8_t>(fn.visibility));
        hashU64(h, fn.modifiers.size());
        for (const std::string& s : fn.modifiers) hashStr(h, s);
        hashU64(h, fn.genericParams.size());
        for (const MetaGenericParam& g : fn.genericParams) hashGeneric(h, g);
        hashU64(h, fn.params.size());
        for (const MetaParam& p : fn.params) hashParam(h, p);
        hashType(h, fn.returnType);
        hashBool(h, fn.returnOwned);
        hashBool(h, fn.isPure);
        hashU64(h, fn.contracts.size());
        for (const MetaContractRecord& c : fn.contracts) hashContract(h, c);
    }
    hashU64(h, md.constants.size());
    for (const MetaConstant& c : md.constants) {
        hashStr(h, c.name);
        hashByte(h, static_cast<std::uint8_t>(c.visibility));
        hashType(h, c.type);
        hashStr(h, c.valueText);
    }
    // §26 (#735): exported extension blocks are public interface — adding,
    // removing, or re-signing an extension member must mark dependents stale.
    // Folded LAST and only when non-empty so a module with no exported
    // extension hashes exactly as it did before the field existed.
    if (!md.extensions.empty()) {
        hashU64(h, md.extensions.size());
        for (const MetaExtension& ext : md.extensions) {
            hashType(h, ext.target);
            hashByte(h, static_cast<std::uint8_t>(ext.visibility));
            if (!ext.genericConstraints.empty()) {
                hashU64(h, ext.genericConstraints.size());
                for (const std::string& c : ext.genericConstraints) hashStr(h, c);
            }
            hashU64(h, ext.members.size());
            for (const MetaMember& m : ext.members) hashMember(h, m);
        }
    }
    return h;
}

// --- Equality ---------------------------------------------------------------

bool MetaContractRecord::operator==(const MetaContractRecord& o) const {
    return kind == o.kind && isStatic == o.isStatic &&
           canonicalText == o.canonicalText && tier == o.tier;
}

bool MetaType::operator==(const MetaType& o) const {
    return kind == o.kind && nullable == o.nullable &&
           primitive == o.primitive && namedName == o.namedName &&
           genericArgs == o.genericArgs && arraySize == o.arraySize &&
           arrayOwned == o.arrayOwned && children == o.children &&
           funcParamMods == o.funcParamMods &&
           funcParamNames == o.funcParamNames &&
           funcReturnOwned == o.funcReturnOwned;
}

bool MetaGenericParam::operator==(const MetaGenericParam& o) const {
    return name == o.name && variance == o.variance &&
           constraintTypes == o.constraintTypes &&
           specialConstraints == o.specialConstraints;
}

bool MetaChoicePayloadField::operator==(const MetaChoicePayloadField& o) const {
    return name == o.name && modifier == o.modifier && type == o.type &&
           hasSerialized == o.hasSerialized &&
           serializedSkip == o.serializedSkip &&
           serializedOptional == o.serializedOptional &&
           serializedName == o.serializedName;
}

bool MetaChoiceCase::operator==(const MetaChoiceCase& o) const {
    return name == o.name && payload == o.payload &&
           hasSerialized == o.hasSerialized &&
           serializedName == o.serializedName;
}

// §9.8 tier 4 (#3179): a polymorphic registry entry's structural equality.
// Backs the round-trip tests the same way MetaChoiceCase does (above).
bool MetaTypeDecl::MetaPolyCase::operator==(
    const MetaTypeDecl::MetaPolyCase& o) const {
    return discriminator == o.discriminator && type == o.type;
}

bool MetaParam::operator==(const MetaParam& o) const {
    return name == o.name && modifier == o.modifier && type == o.type &&
           hasDefault == o.hasDefault && defaultText == o.defaultText &&
           outNullWhen == o.outNullWhen;
}

// §11.3 / §9.2 (issue #501 / #502): MetaMember copy — needed because
// MetaMember is move-only by default (unique_ptr member) but equality tests
// and the deserializer need value copies (readMemberBody pushes into a
// pre-sized vector via assignment, not emplacement).
MetaMember::MetaMember(const MetaMember& o)
    : kind(o.kind), name(o.name), visibility(o.visibility),
      modifiers(o.modifiers), type(o.type), valueText(o.valueText),
      ctorBodyText(o.ctorBodyText), methodBodyText(o.methodBodyText),
      fieldInitText(o.fieldInitText),
      // fix #2893 (§14.1 / §23 / §2.8.2 / §2.8.3 rule 3): property /
      // indexer body source text — additive to the existing body-text
      // trio (ctor/method/fieldInit), same staleness role.
      propertyBodyText(o.propertyBodyText),
      isPure(o.isPure), hasSetter(o.hasSetter),
      setterVisibility(o.setterVisibility), isAutoProperty(o.isAutoProperty),
      hasGetter(o.hasGetter), isEventStatic(o.isEventStatic),
      isEventAsync(o.isEventAsync), isPositional(o.isPositional),
      posParamBodyRef(o.posParamBodyRef),
      contracts(o.contracts),
      genericParams(o.genericParams), params(o.params),
      isOperator(o.isOperator), returnOwned(o.returnOwned),
      hasEnumValue(o.hasEnumValue), enumValue(o.enumValue),
      enumWireName(o.enumWireName),
      serializedSkip(o.serializedSkip),
      serializedOptional(o.serializedOptional),
      serializedWireName(o.serializedWireName) {
    if (o.nestedType)
        nestedType = std::make_unique<MetaTypeDecl>(*o.nestedType);
}

MetaMember& MetaMember::operator=(const MetaMember& o) {
    if (this != &o) {
        kind = o.kind; name = o.name; visibility = o.visibility;
        modifiers = o.modifiers; type = o.type; valueText = o.valueText;
        ctorBodyText = o.ctorBodyText; methodBodyText = o.methodBodyText;
        fieldInitText = o.fieldInitText;
        // fix #2893 (§14.1 / §23 / §2.8.2 / §2.8.3 rule 3): property
        // / indexer body source text — additive to the existing
        // body-text trio, same staleness role.
        propertyBodyText = o.propertyBodyText;
        isPure = o.isPure; hasSetter = o.hasSetter;
        setterVisibility = o.setterVisibility;
        isAutoProperty = o.isAutoProperty; hasGetter = o.hasGetter;
        isEventStatic = o.isEventStatic; isEventAsync = o.isEventAsync;
        isPositional = o.isPositional;
        posParamBodyRef = o.posParamBodyRef;
        contracts = o.contracts;
        genericParams = o.genericParams; params = o.params;
        isOperator = o.isOperator; returnOwned = o.returnOwned;
        hasEnumValue = o.hasEnumValue; enumValue = o.enumValue;
        enumWireName = o.enumWireName;
        serializedSkip = o.serializedSkip;
        serializedOptional = o.serializedOptional;
        serializedWireName = o.serializedWireName;
        nestedType = o.nestedType
            ? std::make_unique<MetaTypeDecl>(*o.nestedType)
            : nullptr;
    }
    return *this;
}

bool MetaMember::operator==(const MetaMember& o) const {
    // Compare nestedType (both null, or both non-null with equal content).
    bool nestedEq = (!nestedType && !o.nestedType) ||
        (nestedType && o.nestedType && *nestedType == *o.nestedType);
    return kind == o.kind && name == o.name && visibility == o.visibility &&
           modifiers == o.modifiers && type == o.type &&
           isPure == o.isPure &&
           // fix #2400 (§23/§2.8.2/§2.8.3 rule 3): hasSetter is part of a
           // Property's signature identity — see the hashMember comment
           // above for why it must compare unequal when it differs (a
           // getter-only vs. read-write property are NOT the same member,
           // structurally or semantically).
           hasSetter == o.hasSetter &&
           // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4): the
           // setter's effective visibility is part of the Property's
           // signature identity — see the hashMember comment above.
           setterVisibility == o.setterVisibility &&
           // fix #2478 (§9.2/§22.8): auto vs. computed form is part of the
           // Property's signature identity for layout purposes — see the
           // hashMember comment above.
           isAutoProperty == o.isAutoProperty &&
           // fix #2461 (§22.3/§23): getter presence is part of the Property's
           // signature identity — see the hashMember comment above.
           hasGetter == o.hasGetter &&
           // fix #2421 (§24/§2.8.2): an Event's static/async
           // classification is part of its signature identity — a static vs.
           // instance event or a sync vs. async event have DIFFERENT struct
           // and itable layouts, so two events that differ on either bit are
           // NOT the same member (see the hashMember comment above).
           isEventStatic == o.isEventStatic &&
           isEventAsync == o.isEventAsync &&
           // fix #2516 (§9.2/§2.8.3 rule 3): isPositional is part of a
           // Field's signature identity — see the hashMember comment above.
           isPositional == o.isPositional &&
           // fix #3221 (§22.10/§2.8.3 rule 3): posParamBodyRef is part of a
           // §22.10 primary-ctor parameter Field's signature identity — see
           // the hashMember comment above.
           posParamBodyRef == o.posParamBodyRef &&
           contracts == o.contracts &&
           genericParams == o.genericParams && params == o.params &&
           isOperator == o.isOperator && returnOwned == o.returnOwned &&
           hasEnumValue == o.hasEnumValue && enumValue == o.enumValue &&
           // fix #3140 (§9.8 / §2.8.3 rule 3): the serialized wire name is part
           // of an EnumConstant's identity — see the hashMember comment above.
           enumWireName == o.enumWireName &&
           // fix #3140 (§9.8 / §2.8.3 rule 3, family F2): the `serialized`
           // clause is part of a storage member's identity — see hashMember.
           serializedSkip == o.serializedSkip &&
           serializedOptional == o.serializedOptional &&
           serializedWireName == o.serializedWireName &&
           valueText == o.valueText && ctorBodyText == o.ctorBodyText &&
           methodBodyText == o.methodBodyText &&
           fieldInitText == o.fieldInitText &&
           // fix #2893 (§14.1 / §23 / §2.8.2 / §2.8.3 rule 3): the
           // property/indexer body source text is part of a Property's
           // signature identity — same reasoning as ctorBodyText /
           // methodBodyText / fieldInitText above (a body change must
           // mark dependents stale, since the importer monomorphizes
           // the body per instantiation).
           propertyBodyText == o.propertyBodyText &&
           nestedEq;
}

bool MetaTypeDecl::operator==(const MetaTypeDecl& o) const {
    return name == o.name && kind == o.kind && visibility == o.visibility &&
           modifiers == o.modifiers && genericParams == o.genericParams &&
           baseTypes == o.baseTypes && members == o.members &&
           invariants == o.invariants &&
           hasPositionalFields == o.hasPositionalFields &&
           dtorBodyText == o.dtorBodyText &&
           builtinExceptionBase == o.builtinExceptionBase &&
           builtinInterfaceBases == o.builtinInterfaceBases &&
           // §35.1 (#3144): the case list is part of a choice's identity.
           choiceCases == o.choiceCases &&
           // §3.6 rule 3 (#3191): the trivial-dispose verdict is part of the
           // public contract, so two metadata values differing only in it are
           // NOT equal — this operator backs the round-trip tests.
           nonTrivialDispose == o.nonTrivialDispose &&
           // §9.8 class shape + polymorphic registry (#3179) are part of the
           // public contract — adding or removing `: ISerializable` (which
           // flips the shape from "" to "tree") is a breaking change for any
           // client that depended on the surface being closed, exactly as a
           // signature change is (§2.8.3 rule 3).
           serializedShape == o.serializedShape &&
           serializedPolyCases == o.serializedPolyCases;
}

bool MetaFunction::operator==(const MetaFunction& o) const {
    return name == o.name && visibility == o.visibility &&
           modifiers == o.modifiers && genericParams == o.genericParams &&
           params == o.params && returnType == o.returnType &&
           returnOwned == o.returnOwned && isPure == o.isPure &&
           contracts == o.contracts;
}

bool MetaConstant::operator==(const MetaConstant& o) const {
    return name == o.name && visibility == o.visibility && type == o.type &&
           valueText == o.valueText;
}

bool MetaExtension::operator==(const MetaExtension& o) const {
    return target == o.target && visibility == o.visibility &&
           members == o.members && sourceSpan == o.sourceSpan &&
           genericConstraints == o.genericConstraints;
}

bool ModuleMetadata::operator==(const ModuleMetadata& o) const {
    return moduleName == o.moduleName && exports == o.exports &&
           types == o.types && functions == o.functions &&
           constants == o.constants && extensions == o.extensions &&  // §26/#735
           isExecutable == o.isExecutable &&
           contractRecordsCarried == o.contractRecordsCarried;  // §2.8.6 (#2054)
}

// --- Serialize --------------------------------------------------------------

namespace {

// A string is emitted as one REC_STRING record whose values are its bytes.
void emitString(llvm::BitstreamWriter& w, const std::string& s) {
    llvm::SmallVector<std::uint64_t, 32> vals;
    vals.reserve(s.size());
    for (char c : s) vals.push_back(static_cast<std::uint8_t>(c));
    w.EmitRecord(REC_STRING, vals);
}

// The same byte payload under a CALLER-CHOSEN record code (#2950). Used for
// records that must be identifiable independently of the plain-REC_STRING
// trailing-string convention.
void emitStringAs(llvm::BitstreamWriter& w, unsigned code,
                  const std::string& s) {
    llvm::SmallVector<std::uint64_t, 32> vals;
    vals.reserve(s.size());
    for (char c : s) vals.push_back(static_cast<std::uint8_t>(c));
    w.EmitRecord(code, vals);
}

// A list of strings emitted as: REC_SCALARS [count], then `count` REC_STRINGs.
void emitStringList(llvm::BitstreamWriter& w, const std::vector<std::string>& v) {
    llvm::SmallVector<std::uint64_t, 1> count{v.size()};
    w.EmitRecord(REC_SCALARS, count);
    for (const std::string& s : v) emitString(w, s);
}

void emitType(llvm::BitstreamWriter& w, const MetaType& t) {
    w.EnterSubblock(METATYPE_BLOCK, 4);
    // scalar header: kind, nullable, primitive, arrayOwned, funcReturnOwned,
    // genericArgs count, children count
    llvm::SmallVector<std::uint64_t, 8> sc{
        static_cast<std::uint64_t>(t.kind),
        t.nullable ? 1u : 0u,
        static_cast<std::uint64_t>(t.primitive),
        t.arrayOwned ? 1u : 0u,
        t.funcReturnOwned ? 1u : 0u,
        t.genericArgs.size(),
        t.children.size(),
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, t.namedName);
    emitString(w, t.arraySize);
    emitStringList(w, t.funcParamMods);
    emitStringList(w, t.funcParamNames);
    for (const MetaType& g : t.genericArgs) emitType(w, g);
    for (const MetaType& c : t.children) emitType(w, c);
    w.ExitBlock();
}

void emitGeneric(llvm::BitstreamWriter& w, const MetaGenericParam& g) {
    w.EnterSubblock(GENERIC_BLOCK, 4);
    llvm::SmallVector<std::uint64_t, 2> sc{g.constraintTypes.size(),
                                           g.specialConstraints.size()};
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, g.name);
    emitString(w, g.variance);
    emitStringList(w, g.specialConstraints);
    for (const MetaType& c : g.constraintTypes) emitType(w, c);
    w.ExitBlock();
}

// §35.1 / §A.3 `CaseField` (#3144): one choice-case payload field. Everything
// inside this block is UNCONDITIONAL — the block itself only exists for a
// choice, so no non-choice module pays a byte for it (the conditionality that
// matters lives at the TYPE_BLOCK level, see emitTypeDecl).
void emitChoicePayloadField(llvm::BitstreamWriter& w,
                            const MetaChoicePayloadField& pf) {
    w.EnterSubblock(CHOICE_PAYLOAD_BLOCK, 4);
    llvm::SmallVector<std::uint64_t, 4> sc{
        pf.hasSerialized ? 1u : 0u,
        pf.serializedSkip ? 1u : 0u,
        pf.serializedOptional ? 1u : 0u,
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, pf.name);
    emitString(w, pf.modifier);
    emitString(w, pf.serializedName);
    emitType(w, pf.type);
    w.ExitBlock();
}

// §35.1 (#3144): one choice case. Declaration order is the case's TAG value, so
// the emission order is the wire contract.
void emitChoiceCase(llvm::BitstreamWriter& w, const MetaChoiceCase& cc) {
    w.EnterSubblock(CHOICE_CASE_BLOCK, 4);
    llvm::SmallVector<std::uint64_t, 2> sc{
        cc.payload.size(),
        cc.hasSerialized ? 1u : 0u,
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, cc.name);
    emitString(w, cc.serializedName);
    for (const MetaChoicePayloadField& pf : cc.payload)
        emitChoicePayloadField(w, pf);
    w.ExitBlock();
}

// §9.8 tier 4 (#3179, family F10 of umbrella #3152): one entry of the
// `polymorphic("d": T, ...)` registry. The block is unconditional inside
// POLY_CASE_BLOCK — the block itself only exists for a polymorphic base, so
// no non-polymorphic module pays a byte for it (the conditionality that
// matters lives at the TYPE_BLOCK level, see emitTypeDecl). Shape mirrors
// emitChoiceCase (scalars, then discriminator, then the registered type).
void emitPolyCase(llvm::BitstreamWriter& w,
                  const MetaTypeDecl::MetaPolyCase& pc) {
    w.EnterSubblock(POLY_CASE_BLOCK, 4);
    w.EmitRecord(REC_SCALARS, llvm::ArrayRef<std::uint64_t>{});
    emitString(w, pc.discriminator);
    emitType(w, pc.type);
    w.ExitBlock();
}

void emitParam(llvm::BitstreamWriter& w, const MetaParam& p) {
    w.EnterSubblock(PARAM_BLOCK, 4);
    // §2392: outNullWhen is a 2nd, additive trailing scalar (see the
    // kSchemaVersion comment in ModuleMetadata.hpp) — old readers only
    // consult sc[0] and ignore the rest.
    llvm::SmallVector<std::uint64_t, 2> sc{
        p.hasDefault ? 1u : 0u,
        static_cast<std::uint64_t>(p.outNullWhen),
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, p.name);
    emitString(w, p.modifier);
    emitString(w, p.defaultText);
    emitType(w, p.type);
    w.ExitBlock();
}

void emitContract(llvm::BitstreamWriter& w, const MetaContractRecord& c) {
    w.EnterSubblock(CONTRACT_BLOCK, 4);
    llvm::SmallVector<std::uint64_t, 3> sc{
        static_cast<std::uint64_t>(c.kind),
        c.isStatic ? 1u : 0u,
        static_cast<std::uint64_t>(c.tier),
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, c.canonicalText);
    w.ExitBlock();
}

// Forward declarations for mutual recursion (MetaMember ↔ MetaTypeDecl).
void emitMember(llvm::BitstreamWriter& w, const MetaMember& m);  // fwd
void emitTypeDecl(llvm::BitstreamWriter& w, const MetaTypeDecl& ty);  // fwd

void emitMember(llvm::BitstreamWriter& w, const MetaMember& m) {
    w.EnterSubblock(MEMBER_BLOCK, 4);
    // fix #2400 (§23/§2.8.2/§2.8.3 rule 4, additive within v7): hasSetter is
    // an 11th trailing scalar, same additive shape as isPure (the 10th) and
    // #2392's PARAM_BLOCK outNullWhen — a pre-#2400 reader's `sc.size() < 8`
    // gate never looks past index 7, so it is unaffected; see the hasSetter
    // field comment in ModuleMetadata.hpp for the full rationale.
    // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4, additive within
    // v7): setterVisibility is a 12th trailing scalar, same additive shape —
    // a pre-#2415 reader's `sc.size() < 8` gate never looks past index 7, so
    // it too is unaffected; see the setterVisibility field comment in
    // ModuleMetadata.hpp for the full rationale.
    // fix #2478 (§9.2/§22.8, additive within v7): isAutoProperty is a 13th
    // trailing scalar, same additive shape — a pre-#2478 reader's
    // `sc.size() < 8` gate never looks past index 7, so it too is
    // unaffected; see the isAutoProperty field comment in ModuleMetadata.hpp
    // for the full rationale.
    // fix #2461 (§22.3/§23, additive within v7): hasGetter is a 14th
    // trailing scalar, same additive shape — a pre-#2461 reader's
    // `sc.size() < 8` gate never looks past index 7, so it too is
    // unaffected; see the hasGetter field comment in ModuleMetadata.hpp for
    // why the missing-scalar default is `true` (not `false` like hasSetter).
    // fix #2421 (§24/§2.8.2, additive within v7): isEventStatic/
    // isEventAsync are 15th/16th trailing scalars, same additive shape — a
    // pre-#2421 reader's `sc.size() < 8` gate never looks past index 7, so it
    // too is unaffected; see the field comments in ModuleMetadata.hpp for why
    // the missing-scalar default is `false` for both (the common instance-
    // sync event classification).
    // fix #2516 (§9.2/§9.3/§2.8.3 rule 4, additive within v7): isPositional
    // is a 17th trailing scalar — same additive shape; a pre-#2516 reader's
    // `sc.size() < 8` gate never looks past index 7, so it too is unaffected.
    // See the isPositional field comment in ModuleMetadata.hpp.
    // fix #3221 (§22.10/§9.8/§2.8.3 rule 4): posParamBodyRef is an 18th
    // trailing scalar — same additive shape; a pre-#3221 reader's `sc.size()
    // < 18` gate never looks past index 17, so it is unaffected. See the
    // posParamBodyRef field comment in ModuleMetadata.hpp.
    llvm::SmallVector<std::uint64_t, 18> sc{
        static_cast<std::uint64_t>(m.kind),
        static_cast<std::uint64_t>(m.visibility),
        m.isOperator ? 1u : 0u,
        m.returnOwned ? 1u : 0u,
        m.hasEnumValue ? 1u : 0u,
        static_cast<std::uint64_t>(m.enumValue),
        m.genericParams.size(),
        m.params.size(),
        m.contracts.size(),
        m.isPure ? 1u : 0u,
        m.hasSetter ? 1u : 0u,
        static_cast<std::uint64_t>(m.setterVisibility),
        m.isAutoProperty ? 1u : 0u,
        m.hasGetter ? 1u : 0u,
        m.isEventStatic ? 1u : 0u,
        m.isEventAsync ? 1u : 0u,
        m.isPositional ? 1u : 0u,
        m.posParamBodyRef ? 1u : 0u,
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, m.name);
    emitStringList(w, m.modifiers);
    emitType(w, m.type);
    for (const MetaGenericParam& g : m.genericParams) emitGeneric(w, g);
    for (const MetaParam& p : m.params) emitParam(w, p);
    for (const MetaContractRecord& c : m.contracts) emitContract(w, c);
    // §22.12 const value (schema v2, issue #328 FINDING [3] BUG A): a trailing
    // REC_STRING after the params. A v1 reader skips it (skip-unknown); a v2
    // reader reads it tolerantly and an absent record yields "" (a v1 blob).
    emitString(w, m.valueText);
    // §14 / #662c (schema v5): for a Constructor member, a second trailing
    // REC_STRING carries the ctor body source text for imported generic classes.
    // A v4 reader hits finishBlockImpl which skips all trailing content after
    // valueText, so the extra string is silently ignored (tolerant skip-unknown).
    // An empty ctorBodyText is still emitted for Constructor members (a zero-
    // length REC_STRING) so readMemberBody can capture it predictably.
    if (m.kind == MetaMemberKind::Constructor)
        emitString(w, m.ctorBodyText);
    // §14.1 / #783 (schema v5 additive): for a Method member of a generic class,
    // a second trailing REC_STRING carries the method body source text. Emitted
    // ONLY when non-empty so a module with no generic methods produces
    // byte-identical metadata (a reader predating this field ignores it for
    // Method members; a v4 reader rejects v5 outright).
    if (m.kind == MetaMemberKind::Method && !m.methodBodyText.empty())
        emitString(w, m.methodBodyText);
    // §2.8.2 / #2351 (schema v7 additive): for a Field member of a generic
    // class with a carryable static initializer, a second trailing
    // REC_STRING carries the initializer EXPRESSION source text. Emitted
    // ONLY when non-empty, mirroring methodBodyText's own additive shape —
    // a module with no generic-class static-field initializer produces
    // byte-identical metadata to before this fix.
    if (m.kind == MetaMemberKind::Field && !m.fieldInitText.empty())
        emitString(w, m.fieldInitText);
    // §14.1 / §23 / §2.8.2 / §2.8.3 rule 4 (#2893, schema v7 additive):
    // for a Property or Indexer member of a GENERIC class or record, a
    // second trailing REC_STRING carries the property/indexer body source
    // text. Emitted ONLY when non-empty, mirroring methodBodyText's own
    // additive shape — a module with no generic-class property/indexer
    // body produces byte-identical metadata to before this fix.
    // (valueText is the FIRST trailing string for any member kind; for a
    // Property that is the auto-property initializer text, #2467; this
    // is the SECOND.)
    if ((m.kind == MetaMemberKind::Property ||
         m.kind == MetaMemberKind::Indexer) &&
        !m.propertyBodyText.empty())
        emitString(w, m.propertyBodyText);
    // fix #3140 (§9.8 / §10, schema v7 additive): for an EnumConstant member
    // carrying a `serialized "<name>"` rename, a second trailing REC_STRING
    // holds the wire name. Emitted ONLY when non-empty, exactly as
    // methodBodyText/fieldInitText are — a module whose enums use no
    // `serialized` clause produces byte-identical metadata to before this fix.
    if (m.kind == MetaMemberKind::EnumConstant && !m.enumWireName.empty())
        emitString(w, m.enumWireName);
    // fix #3140 (§9.8, family F2 of umbrella #3152; schema v7 additive): a
    // storage member's `serialized` clause, under its OWN record code so it is
    // independent of the trailing REC_STRINGs above (either may be absent, and
    // neither can be read as the other, in either order). Emitted ONLY when the
    // member actually carries a clause, so a module using none is byte-identical
    // to before this fix — and `hashMember` is guarded by the SAME predicate,
    // which is the half c97a49f had to correct for this family's F4 slice.
    if (m.serializedSkip || m.serializedOptional ||
        !m.serializedWireName.empty()) {
        llvm::SmallVector<std::uint64_t, 8> sv{
            m.serializedSkip ? 1u : 0u,
            m.serializedOptional ? 1u : 0u,
        };
        for (char c : m.serializedWireName)
            sv.push_back(static_cast<std::uint8_t>(c));
        w.EmitRecord(REC_SER_CLAUSE, sv);
    }
    // fix #3202 (§22.10 / §22.5 / §2.8.3 rule 4, v0.254): a positional
    // primary-ctor parameter's default-value expression text, under its
    // OWN record code (REC_POSPARAM_DEFAULT, see enum RecordCode above).
    // Emitted only when non-empty, mirroring the fieldInitText /
    // methodBodyText / enumWireName / REC_SER_CLAUSE pattern above — a
    // module whose primary parameters carry no default is byte-identical
    // to before. `hashMember` is guarded by the SAME predicate, the
    // property the F4 follow-up (c97a49f) had to correct for REC_SER_CLAUSE
    // and that every subsequent additive record since then has held.
    if (!m.posParamDefaultText.empty()) {
        llvm::SmallVector<std::uint64_t, 64> dv;
        dv.reserve(m.posParamDefaultText.size());
        for (char c : m.posParamDefaultText)
            dv.push_back(static_cast<std::uint8_t>(c));
        w.EmitRecord(REC_POSPARAM_DEFAULT, dv);
    }
    // §11.3 (schema v4, issue #502): when kind == NestedType, emit the nested
    // type's full declaration as an embedded TYPE_BLOCK. The trailing
    // finishBlockImpl in readMemberBody skips it for v3 readers (they never
    // see v4 blobs due to the version check). For v4 readers with kind !=
    // NestedType the block is absent; readMemberBody ignores it.
    if (m.kind == MetaMemberKind::NestedType && m.nestedType)
        emitTypeDecl(w, *m.nestedType);
    w.ExitBlock();
}

void emitTypeDecl(llvm::BitstreamWriter& w, const MetaTypeDecl& ty) {
    w.EnterSubblock(TYPE_BLOCK, 4);
    // §9.2 (schema v4, issue #501): add hasPositionalFields as the 6th scalar.
    // v3 readers reject v4 blobs so they never see this; v4 readers tolerate
    // a v3 blob where sc.size() == 5 (hasPositionalFields treated as false).
    llvm::SmallVector<std::uint64_t, 8> sc{
        static_cast<std::uint64_t>(ty.kind),
        static_cast<std::uint64_t>(ty.visibility),
        ty.genericParams.size(),
        ty.baseTypes.size(),
        ty.members.size(),
        ty.hasPositionalFields ? 1u : 0u,
        ty.invariants.size(),
    };
    // #3139 (additive): an 8th scalar counts the built-in-interface bases
    // emitted after the invariants. A reader predating it stops at sc[6] and
    // its finishBlock skips the trailing subblocks.
    //
    // ⚠⚠ READ THIS BEFORE ADDING A 9th SCALAR. This is the first CONDITIONAL
    // *POSITIONAL* entry in REC_SCALARS, and that makes it different in kind
    // from #2950's REC_EXC_BASE, which is cited as its template: REC_EXC_BASE is
    // a separately-CODED record, so its presence is self-describing and its
    // absence costs nothing. Here, position 7 means "nBuiltinIfaces" ONLY when
    // the 8th entry was emitted at all. So a future 9th scalar MUST be either
    //   (a) conditional on the 8th being present, or
    //   (b) a distinct record code rather than a positional scalar (preferred).
    // An unconditional 9th entry would land at index 7 for a type with no
    // built-in interface, and `readTypeDeclBody` would silently read that
    // foreign value as `nBuiltinIfaces` and then try to read that many
    // METATYPE_BLOCKs. That is a silent misparse of an artifact, not a build
    // failure — the worst failure mode this format has.
    //
    // CONDITIONAL, and it has to be — this is what makes a module whose types
    // declare no built-in interface produce byte-identical metadata, the
    // property #2950's REC_EXC_BASE has and the property the field comment in
    // ModuleMetadata.hpp claims. An unconditional 8th entry is not free
    // bookkeeping: it widens REC_SCALARS for EVERY type in every module, and
    // (with the matching fold in hashTypeDecl) moves the VBR-encoded hash in
    // the header, which shifts every byte after it. MEASURED before this guard:
    // a module with no built-in-interface base anywhere had its metadata
    // perturbed. ⚠ The BYTE DELTA is probe-specific — three separate probes gave
    // 468->476, 720->716 and 672->672 — so do not quote one as if it were the
    // property. The REPRODUCIBLE property is: with the guards, a module declaring
    // no built-in interface is BYTE-IDENTICAL to a pre-#3139 build; without them,
    // the emitted length can match while the CONTENT HASH still differs (verified
    // by a one-variable A/B that removed only these two guards).
    // `readTypeDeclBody` already tolerates a 7-entry record (`sc.size() >= 8`),
    // so nothing on the read side changes.
    if (!ty.builtinInterfaceBases.empty())
        sc.push_back(ty.builtinInterfaceBases.size());
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, ty.name);
    emitStringList(w, ty.modifiers);
    for (const MetaGenericParam& g : ty.genericParams) emitGeneric(w, g);
    for (const MetaType& b : ty.baseTypes) emitType(w, b);
    for (const MetaMember& m : ty.members) emitMember(w, m);
    for (const MetaContractRecord& c : ty.invariants) emitContract(w, c);
    // §9.8 / §18.5 / §21.4 (#3139): the built-in-interface bases, counted by
    // sc[7]. Emitted after the invariants, so every count-driven list a reader
    // predating this field knows about has already been consumed when it stops.
    for (const MetaType& b : ty.builtinInterfaceBases) emitType(w, b);
    // §14 / #662c (schema v5): for a GENERIC class, a trailing REC_STRING after
    // the members carries the destructor body source text (empty = no user body).
    // v4 readers encounter finishBlock which skips all trailing content, so this
    // is silently ignored (tolerant skip-unknown). Only emit for generic classes.
    if (!ty.genericParams.empty())
        emitString(w, ty.dtorBodyText);
    // §12 / §31.4 (#2950): the built-in exception base, under its own record
    // code so it is independent of the trailing dtorBodyText string above.
    // Emitted only when non-empty, so a module with no exception subclass
    // produces byte-identical metadata to before.
    if (!ty.builtinExceptionBase.empty())
        emitStringAs(w, REC_EXC_BASE, ty.builtinExceptionBase);
    // §35.1 (#3144): the choice's cases, as trailing CHOICE_CASE_BLOCKs with NO
    // count scalar. Emitted LAST, after both trailing capture records, so an
    // OLD reader's finishBlock sees the dtorBodyText REC_STRING and the
    // REC_EXC_BASE record in exactly the positions it did before and then
    // SkipBlock()s subblock ids it does not know — its result is byte-for-byte
    // what it was. Emitted only for a choice (the list is empty for every other
    // kind), which — with the matching guard in hashTypeDecl — is what keeps a
    // module with no choice BYTE-IDENTICAL to a pre-#3144 build.
    for (const MetaChoiceCase& cc : ty.choiceCases) emitChoiceCase(w, cc);
    // §3.6 / §28.9 / §21.9 (#3191): the trivial-dispose verdict, inverted, under
    // its own record code. Emitted only when TRUE — the #2950/#3139/#3144 rule —
    // so a module whose types are all trivial-dispose (the overwhelming majority)
    // produces byte-identical metadata to before. Position is immaterial to
    // correctness because the code is self-describing and every reader this
    // format has had `skipRecord()`s an unknown one; it is emitted LAST so the
    // trailing capture records above keep the exact positions they had.
    if (ty.nonTrivialDispose)
        w.EmitRecord(REC_DISPOSE_VERDICT, llvm::ArrayRef<std::uint64_t>{1});
    // §9.8 / §2.8.3 rule 4 (#3179, family F10 of umbrella #3152): the class's
    // serialized CLASS SHAPE, as the bytes of one of A.3's three identifiers
    // ("tree" / "graph" / "cyclic"), under its OWN record code so the
    // presence / absence is self-describing and a reader predating the field
    // `skipRecord()`s it, leaving the field empty (the pre-fix reading, which
    // is exactly what the §9.8 surface admits for a non-serializable class).
    // Conditional on non-empty — empty for every non-class kind and for a
    // class that declares no ISerializable / IDeserializable conformance. The
    // matching `hashTypeDecl` fold is conditional on the same predicate, with
    // the same rationale as the prior #2950 / #3139 / #3144 / #3191 records.
    // Position after the existing trailing records is immaterial because the
    // code is self-describing — emit LAST so the previous captures keep the
    // exact positions they had.
    if (!ty.serializedShape.empty()) {
        llvm::SmallVector<std::uint64_t, 8> ss;
        ss.reserve(ty.serializedShape.size());
        for (char c : ty.serializedShape)
            ss.push_back(static_cast<std::uint8_t>(c));
        w.EmitRecord(REC_SER_TYPE_SHAPE, ss);
    }
    for (const MetaTypeDecl::MetaPolyCase& pc : ty.serializedPolyCases)
        emitPolyCase(w, pc);
    // §9.8 tier 4 / §2.8.3 rule 4 (#3179): the `polymorphic(...)` registry
    // entries. Same trailing-block pattern as #3144's `choiceCases` — one
    // POLY_CASE_BLOCK per entry, no count scalar (see the ⚠ block above the
    // 8th scalar for why a 9th positional scalar was not an option, in
    // particular on a class that declares no built-in interface: the reader
    // would silently read it as `nBuiltinIfaces` and try to read that many
    // METATYPE_BLOCKs, a silent misparse). `skipRecord()`s for any reader
    // predating the block id leave an empty registry on the class — the
    // pre-fix reading, and the correct one for a non-tier-4 class. Conditional
    // on non-empty for byte-identical metadata on the unaffected majority.
    w.ExitBlock();
}

void emitFunction(llvm::BitstreamWriter& w, const MetaFunction& fn) {
    w.EnterSubblock(FUNC_BLOCK, 4);
    llvm::SmallVector<std::uint64_t, 6> sc{
        static_cast<std::uint64_t>(fn.visibility),
        fn.returnOwned ? 1u : 0u,
        fn.genericParams.size(),
        fn.params.size(),
        fn.contracts.size(),
        fn.isPure ? 1u : 0u,
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, fn.name);
    emitStringList(w, fn.modifiers);
    emitType(w, fn.returnType);
    for (const MetaGenericParam& g : fn.genericParams) emitGeneric(w, g);
    for (const MetaParam& p : fn.params) emitParam(w, p);
    for (const MetaContractRecord& c : fn.contracts) emitContract(w, c);
    w.ExitBlock();
}

// §26 (#735): one exported extension block. Shape mirrors emitTypeDecl (scalars,
// then the target type, then the members) so the reader is symmetric.
void emitExtension(llvm::BitstreamWriter& w, const MetaExtension& ext) {
    w.EnterSubblock(EXTENSION_BLOCK, 4);
    // §26.5 rule 4 (#2329): the trailing five scalars are additive diagnostic
    // metadata (span + constraint count). A pre-#2329 reader consumes only the
    // first two and skips the trailing strings when it finishes this block.
    llvm::SmallVector<std::uint64_t, 7> sc{
        static_cast<std::uint64_t>(ext.visibility),
        ext.members.size(),
        ext.sourceSpan.beginLine,
        ext.sourceSpan.beginColumn,
        ext.sourceSpan.endLine,
        ext.sourceSpan.endColumn,
        ext.genericConstraints.size(),
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitType(w, ext.target);
    for (const MetaMember& m : ext.members) emitMember(w, m);
    emitString(w, ext.sourceSpan.file);
    for (const std::string& c : ext.genericConstraints) emitString(w, c);
    w.ExitBlock();
}

void emitConstant(llvm::BitstreamWriter& w, const MetaConstant& c) {
    w.EnterSubblock(CONST_BLOCK, 4);
    llvm::SmallVector<std::uint64_t, 1> sc{
        static_cast<std::uint64_t>(c.visibility)};
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, c.name);
    emitString(w, c.valueText);
    emitType(w, c.type);
    w.ExitBlock();
}

// Shared writer body: emits the whole MODULE_BLOCK. When `withUnknownExtras`,
// also emits an unknown record code + an unknown subblock id to exercise the
// reader's skip-unknown path (issue #328 forward-compat test). The
// `producerRevision` is the producer's content-revision stamp; the production
// serializers pass `kProducerContentRevision`, and the test-only
// `serializeWithProducerRevision` passes a custom value to forge a stale
// artifact for the #3198 regression test.
std::vector<std::uint8_t> serializeImpl(const ModuleMetadata& md,
                                        bool withUnknownExtras,
                                        std::uint32_t producerRevision) {
    llvm::SmallVector<char, 0> buffer;
    {
        llvm::BitstreamWriter w(buffer);
        w.EnterSubblock(MODULE_BLOCK, 4);

        // Header record first (issue #328: magic + schema version + hash; issue
        // #3198: producer content-revision stamp). The stamp is the 5th value
        // — additive, no schema bump (the existing reader's `hdr.size() < 4`
        // gate never looks past index 3, so a stamp-bearing blob is read by
        // an old reader exactly as a stamp-less one was). See the constant's
        // comment in ModuleMetadata.hpp for the rejection semantics.
        std::uint64_t hash = semanticHash(md);
        llvm::SmallVector<std::uint64_t, 5> hdr{
            kMetadataMagic, kSchemaVersion,
            hash & 0xFFFFFFFFu, (hash >> 32) & 0xFFFFFFFFu,
            producerRevision};
        w.EmitRecord(REC_HEADER, hdr);

        if (withUnknownExtras) {
            // An unknown record code (a future schema's optional record): the
            // current reader must skipRecord() it.
            llvm::SmallVector<std::uint64_t, 3> junk{111, 222, 333};
            w.EmitRecord(REC_UNKNOWN_FUTURE, junk);
            // An unknown subblock id (a future schema's optional subblock): the
            // current reader must SkipBlock() it.
            w.EnterSubblock(UNKNOWN_FUTURE_BLOCK, 4);
            llvm::SmallVector<std::uint64_t, 2> inner{1, 2};
            w.EmitRecord(REC_SCALARS, inner);
            emitString(w, std::string("future"));
            w.ExitBlock();
        }

        // Module name (§2.1) as a single REC_STRING, then exports (§2.4).
        emitString(w, md.moduleName);
        {
            llvm::SmallVector<std::uint64_t, 1> ec{md.exports.size()};
            w.EmitRecord(REC_SCALARS, ec); // export count
            for (const std::string& e : md.exports) emitString(w, e);
        }
        {
            // §2.8.4 rule 4 (issue #1733, schema v6): isExecutable rides as a
            // 4th scalar after the three top-level counts. A v<=5 reader reads
            // only the first three (it never sees a v6 blob — the version bump
            // makes it reject one); a v6 reader reads the flag, and a v<=5 BLOB
            // read by a v6 reader has only three scalars → isExecutable = false
            // (library), preserving back-compat for existing library artifacts.
            llvm::SmallVector<std::uint64_t, 6> counts{
                md.types.size(), md.functions.size(), md.constants.size(),
                md.isExecutable ? 1u : 0u,
                md.contractRecordsCarried ? 1u : 0u};
            // §26 (#735, additive): the extension count rides as a 6th scalar,
            // emitted ONLY when there is at least one exported extension, so a
            // module without extensions produces byte-identical metadata to
            // before the field existed (an absent 6th scalar reads as 0).
            if (!md.extensions.empty()) counts.push_back(md.extensions.size());
            w.EmitRecord(REC_SCALARS, counts);
        }
        for (const MetaTypeDecl& ty : md.types) emitTypeDecl(w, ty);
        for (const MetaFunction& fn : md.functions) emitFunction(w, fn);
        for (const MetaConstant& c : md.constants) emitConstant(w, c);
        for (const MetaExtension& ext : md.extensions) emitExtension(w, ext);

        w.ExitBlock();
    }
    std::vector<std::uint8_t> out(buffer.size());
    std::memcpy(out.data(), buffer.data(), buffer.size());
    return out;
}

} // namespace

std::vector<std::uint8_t> serialize(const ModuleMetadata& md) {
    return serializeImpl(md, /*withUnknownExtras=*/false, kProducerContentRevision);
}

std::vector<std::uint8_t> serializeWithUnknownExtras(const ModuleMetadata& md) {
    return serializeImpl(md, /*withUnknownExtras=*/true, kProducerContentRevision);
}

// §2.5 / §2.8.2 (issue #3198, v0.254): test-only writer that takes an explicit
// producer stamp. Used by the #3198 regression test to forge a "stale" artifact
// (stamp != kProducerContentRevision) and verify the loader's rejection path.
// Production callers always go through `serialize` / `serializeWithUnknownExtras`,
// which stamp `kProducerContentRevision` — there is no other call site.
std::vector<std::uint8_t> serializeWithProducerRevision(const ModuleMetadata& md,
                                                       std::uint32_t stamp) {
    return serializeImpl(md, /*withUnknownExtras=*/false, stamp);
}

// --- Deserialize ------------------------------------------------------------

namespace {

// A small reader wrapper that turns LLVM's Expected<...> into bool-returning
// helpers, so the decode body reads top-to-bottom without exception-style
// error plumbing.
struct Reader {
    llvm::BitstreamCursor cursor;
    bool ok = true;

    explicit Reader(const std::vector<std::uint8_t>& bytes)
        : cursor(llvm::ArrayRef<std::uint8_t>(bytes.data(), bytes.size())) {}

    void fail() { ok = false; }
};

// The kind of entry advance() found. For a Record, `id` is the bitstream
// ABBREV id (always bitc::UNABBREV_RECORD here, since this format abbreviates
// nothing) — NOT the record code. The record code is the first value the
// reader sees, recovered by readRecord()'s return value; callers must read the
// record to learn its code. For a SubBlock, `id` is the block id.
enum class Next { Record, SubBlock, EndBlock, Error };
Next nextEntry(Reader& r, unsigned& id) {
    auto e = r.cursor.advance(llvm::BitstreamCursor::AF_DontPopBlockAtEnd);
    if (!e) { llvm::consumeError(e.takeError()); return Next::Error; }
    switch (e->Kind) {
        case llvm::BitstreamEntry::Record: id = e->ID; return Next::Record;
        case llvm::BitstreamEntry::SubBlock: id = e->ID; return Next::SubBlock;
        case llvm::BitstreamEntry::EndBlock: return Next::EndBlock;
        default: return Next::Error;
    }
}

// Read the next record, regardless of code, returning its record code in `code`
// and its values in `vals`. Skips any nested subblock encountered first (a
// future schema's unknown subblock — issue #328 skip-unknown). Returns false on
// EndBlock or error. The caller dispatches on `code`; an UNKNOWN code is simply
// ignored by the caller, which keeps looking — this is the read-older path.
bool readNextRecord(Reader& r, unsigned& code,
                    llvm::SmallVectorImpl<std::uint64_t>& vals) {
    while (true) {
        unsigned id = 0;
        Next n = nextEntry(r, id);
        if (n == Next::Record) {
            vals.clear();
            auto got = r.cursor.readRecord(id, vals);
            if (!got) { llvm::consumeError(got.takeError()); return false; }
            code = got.get();
            return true;
        }
        if (n == Next::SubBlock) {
            if (r.cursor.SkipBlock()) return false; // unknown subblock: skip
            continue;
        }
        return false; // EndBlock or Error
    }
}

// Read the next REC_STRING; skip unknown records/subblocks before it
// (forward-compat). Returns false on EndBlock/error.
bool expectString(Reader& r, std::string& out) {
    while (true) {
        unsigned code = 0;
        llvm::SmallVector<std::uint64_t, 32> vals;
        if (!readNextRecord(r, code, vals)) return false;
        if (code != REC_STRING) continue; // unknown record: skip, keep looking
        out.clear();
        out.reserve(vals.size());
        for (std::uint64_t v : vals) out.push_back(static_cast<char>(v & 0xFF));
        return true;
    }
}

// Read the next REC_SCALARS; skip unknown records/subblocks before it. Returns
// false on EndBlock/error.
bool expectScalars(Reader& r, llvm::SmallVectorImpl<std::uint64_t>& vals) {
    while (true) {
        unsigned code = 0;
        if (!readNextRecord(r, code, vals)) return false;
        if (code != REC_SCALARS) continue; // unknown record: skip
        return true;
    }
}

// Read a string-list (REC_SCALARS [count] then count REC_STRINGs).
bool readStringList(Reader& r, std::vector<std::string>& out) {
    llvm::SmallVector<std::uint64_t, 1> count;
    if (!expectScalars(r, count) || count.empty()) return false;
    out.clear();
    out.resize(count[0]);
    for (std::uint64_t i = 0; i < count[0]; ++i) {
        if (!expectString(r, out[i])) return false;
    }
    return true;
}

// Enter the next subblock that has id `wantId`, skipping unknown records and
// unknown subblocks before it. On success the cursor is positioned inside the
// subblock. Returns false on EndBlock/error.
bool enterSubblock(Reader& r, unsigned wantId) {
    while (true) {
        unsigned id = 0;
        Next n = nextEntry(r, id);
        if (n == Next::SubBlock) {
            if (id == wantId) {
                if (r.cursor.EnterSubBlock(id)) return false;
                return true;
            }
            if (r.cursor.SkipBlock()) return false; // unknown subblock: skip
            continue;
        }
        if (n == Next::Record) {
            // Unknown leading record: read (and discard) it, then keep looking.
            // (skipRecord needs the abbrev id, which `id` already is.)
            auto sk = r.cursor.skipRecord(id);
            if (!sk) { llvm::consumeError(sk.takeError()); return false; }
            continue;
        }
        return false; // EndBlock or Error
    }
}

// Consume to the END_BLOCK of the current subblock, skipping anything left
// (unknown trailing records / subblocks). Leaves the cursor just past the
// END_BLOCK. Returns false on error. When `captureString` is non-null, the
// FIRST trailing REC_STRING encountered before END_BLOCK is read into it (the
// rest are still skipped); this is how the schema-v2 MEMBER_BLOCK reads the
// optional trailing §22.12 const `valueText` TOLERANTLY: a v1 blob has no such
// trailing record, so `*captureString` is left untouched (empty) and the block
// finishes exactly as before (issue #328 FINDING [3] BUG A).
//
// #2950: `codedCapture` is the same tolerant capture keyed on a record CODE
// instead of on position — the FIRST trailing record whose code is
// `capturedCode` is read into it. The two captures are independent: a blob may
// carry either, both (in either order), or neither, and a reader predating a
// coded record simply skips it. That independence is the point — the trailing
// REC_STRING convention can only ever express ONE optional string per block.
//
// §35.1 (#3144): `choiceCases` is the third, subblock-keyed capture — the FIRST
// two are record-keyed and so structurally cannot express a repeated,
// count-less list. When it is non-null, every trailing CHOICE_CASE_BLOCK is
// entered and appended IN ORDER (declaration order is the case's tag value);
// when it is null — every caller but readTypeDeclBody — the block is
// SkipBlock()'d exactly as an unknown subblock id always was, which is also
// precisely what a reader predating this field does.
bool readChoiceCaseBody(Reader& r, MetaChoiceCase& cc);  // fwd (#3144)
bool readPolyCaseBody(Reader& r,
                      MetaTypeDecl::MetaPolyCase& pc);  // fwd (#3179)

bool finishBlockImpl(Reader& r, std::string* captureString,
                     unsigned capturedCode = 0,
                     std::string* codedCapture = nullptr,
                     std::vector<MetaChoiceCase>* choiceCases = nullptr,
                     bool* disposeVerdict = nullptr,
                     // §9.8 class shape + polymorphic registry (#3179,
                     // family F10 of umbrella #3152): a SECOND coded string
                     // capture for REC_SER_TYPE_SHAPE (independent of the
                     // existing `codedCapture`, which is already REC_EXC_BASE
                     // in the readTypeDeclBody call site). `polyCases` rides
                     // a subblock id and follows the same `choiceCases`
                     // optional-pointer shape. Both are nullptrable so this
                     // stays a strict extension of the previous 5-argument
                     // API.
                     unsigned secondCapturedCode = 0,
                     std::string* secondCodedCapture = nullptr,
                     std::vector<MetaTypeDecl::MetaPolyCase>* polyCases =
                         nullptr) {
    while (true) {
        unsigned id = 0;
        Next n = nextEntry(r, id);
        if (n == Next::EndBlock) {
            // AF_DontPopBlockAtEnd was used, so pop the scope now.
            if (r.cursor.ReadBlockEnd()) return false;
            return true;
        }
        if (n == Next::Record) {
            if (captureString != nullptr || codedCapture != nullptr ||
                disposeVerdict != nullptr) {
                // Read this record; capture it if it is one we are waiting for.
                llvm::SmallVector<std::uint64_t, 32> vals;
                auto got = r.cursor.readRecord(id, vals);
                if (!got) { llvm::consumeError(got.takeError()); return false; }
                const unsigned code = got.get();
                // §3.6 / §28.9 (#3191): the trivial-dispose verdict, a single
                // scalar rather than a string payload, so it is captured here
                // rather than through the `dst` byte-copy below. Absent — an
                // artifact written before this field, or a type that is
                // trivial-dispose — leaves the caller's `false` untouched, which
                // is exactly the pre-fix reading.
                if (disposeVerdict != nullptr && code == REC_DISPOSE_VERDICT) {
                    *disposeVerdict = !vals.empty() && vals[0] != 0;
                    disposeVerdict = nullptr;
                    continue;
                }
                std::string* dst = nullptr;
                if (captureString != nullptr && code == REC_STRING) {
                    dst = captureString;
                    captureString = nullptr;  // capture only the first string
                } else if (codedCapture != nullptr && code == capturedCode) {
                    dst = codedCapture;
                    codedCapture = nullptr;
                } else if (secondCodedCapture != nullptr &&
                           code == secondCapturedCode) {
                    // §9.8 (#3179): the second coded-capture slot. Read it
                    // the same way the existing one is read (decode bytes
                    // back into a std::string), set the pointer to nullptr
                    // so a second REC_SER_TYPE_SHAPE — which #3191's same
                    // "last wins" precedent does NOT predict, since at most
                    // one such record exists per type — does not silently
                    // overwrite the first; absent leaves the field empty,
                    // the pre-fix reading.
                    dst = secondCodedCapture;
                    secondCodedCapture = nullptr;
                }
                if (dst != nullptr) {
                    dst->clear();
                    dst->reserve(vals.size());
                    for (std::uint64_t v : vals)
                        dst->push_back(static_cast<char>(v & 0xFF));
                }
                continue;
            }
            auto sk = r.cursor.skipRecord(id);
            if (!sk) { llvm::consumeError(sk.takeError()); return false; }
            continue;
        }
        if (n == Next::SubBlock) {
            // §35.1 (#3144): a trailing choice case, when the caller wants them.
            if (choiceCases != nullptr && id == CHOICE_CASE_BLOCK) {
                if (r.cursor.EnterSubBlock(CHOICE_CASE_BLOCK)) return false;
                choiceCases->emplace_back();
                if (!readChoiceCaseBody(r, choiceCases->back())) return false;
                continue;
            }
            // §9.8 tier 4 (#3179): a trailing polymorphic registry entry,
            // when the caller wants them. Same self-describing subblock-id
            // pattern as CHOICE_CASE_BLOCK (no count scalar — see the ⚠
            // block on emitTypeDecl's 8th scalar for why a 9th positional
            // scalar would silently misparse as #3139's `nBuiltinIfaces`).
            if (polyCases != nullptr && id == POLY_CASE_BLOCK) {
                if (r.cursor.EnterSubBlock(POLY_CASE_BLOCK)) return false;
                polyCases->emplace_back();
                if (!readPolyCaseBody(r, polyCases->back())) return false;
                continue;
            }
            if (r.cursor.SkipBlock()) return false;
            continue;
        }
        return false;
    }
}

bool finishBlock(Reader& r) { return finishBlockImpl(r, /*captureString=*/nullptr); }

bool readType(Reader& r, MetaType& t); // fwd

bool readTypeBody(Reader& r, MetaType& t) {
    llvm::SmallVector<std::uint64_t, 8> sc;
    if (!expectScalars(r, sc) || sc.size() < 7) return false;
    t.kind = static_cast<TypeKind>(sc[0]);
    t.nullable = sc[1] != 0;
    t.primitive = static_cast<PrimitiveKind>(sc[2]);
    t.arrayOwned = sc[3] != 0;
    t.funcReturnOwned = sc[4] != 0;
    std::uint64_t nGeneric = sc[5];
    std::uint64_t nChildren = sc[6];
    if (!expectString(r, t.namedName)) return false;
    if (!expectString(r, t.arraySize)) return false;
    if (!readStringList(r, t.funcParamMods)) return false;
    if (!readStringList(r, t.funcParamNames)) return false;
    t.genericArgs.resize(nGeneric);
    for (std::uint64_t i = 0; i < nGeneric; ++i) {
        if (!readType(r, t.genericArgs[i])) return false;
    }
    t.children.resize(nChildren);
    for (std::uint64_t i = 0; i < nChildren; ++i) {
        if (!readType(r, t.children[i])) return false;
    }
    return finishBlock(r);
}

bool readType(Reader& r, MetaType& t) {
    if (!enterSubblock(r, METATYPE_BLOCK)) return false;
    return readTypeBody(r, t);
}

bool readGenericBody(Reader& r, MetaGenericParam& g) {
    llvm::SmallVector<std::uint64_t, 2> sc;
    if (!expectScalars(r, sc) || sc.size() < 2) return false;
    std::uint64_t nConstraints = sc[0];
    if (!expectString(r, g.name)) return false;
    if (!expectString(r, g.variance)) return false;
    if (!readStringList(r, g.specialConstraints)) return false;
    g.constraintTypes.resize(nConstraints);
    for (std::uint64_t i = 0; i < nConstraints; ++i) {
        if (!readType(r, g.constraintTypes[i])) return false;
    }
    return finishBlock(r);
}

bool readGeneric(Reader& r, MetaGenericParam& g) {
    if (!enterSubblock(r, GENERIC_BLOCK)) return false;
    return readGenericBody(r, g);
}

bool readParamBody(Reader& r, MetaParam& p) {
    llvm::SmallVector<std::uint64_t, 2> sc;
    if (!expectScalars(r, sc) || sc.empty()) return false;
    p.hasDefault = sc[0] != 0;
    // §2392: outNullWhen is a 2nd, additive trailing scalar — absent (a
    // pre-#2392 blob) reads as 0/None, matching every such blob's implicit
    // value before this field existed.
    p.outNullWhen = sc.size() >= 2 ? static_cast<std::uint8_t>(sc[1]) : 0;
    if (!expectString(r, p.name)) return false;
    if (!expectString(r, p.modifier)) return false;
    if (!expectString(r, p.defaultText)) return false;
    if (!readType(r, p.type)) return false;
    return finishBlock(r);
}

bool readParam(Reader& r, MetaParam& p) {
    if (!enterSubblock(r, PARAM_BLOCK)) return false;
    return readParamBody(r, p);
}

// §35.1 / §A.3 `CaseField` (#3144): one choice-case payload field. The caller
// has already entered CHOICE_PAYLOAD_BLOCK.
bool readChoicePayloadFieldBody(Reader& r, MetaChoicePayloadField& pf) {
    llvm::SmallVector<std::uint64_t, 4> sc;
    if (!expectScalars(r, sc) || sc.size() < 3) return false;
    pf.hasSerialized = sc[0] != 0;
    pf.serializedSkip = sc[1] != 0;
    pf.serializedOptional = sc[2] != 0;
    if (!expectString(r, pf.name)) return false;
    if (!expectString(r, pf.modifier)) return false;
    if (!expectString(r, pf.serializedName)) return false;
    if (!readType(r, pf.type)) return false;
    return finishBlock(r);
}

// §35.1 (#3144): one choice case. The caller has already entered
// CHOICE_CASE_BLOCK.
bool readChoiceCaseBody(Reader& r, MetaChoiceCase& cc) {
    llvm::SmallVector<std::uint64_t, 2> sc;
    if (!expectScalars(r, sc) || sc.size() < 2) return false;
    const std::uint64_t nPayload = sc[0];
    cc.hasSerialized = sc[1] != 0;
    if (!expectString(r, cc.name)) return false;
    if (!expectString(r, cc.serializedName)) return false;
    cc.payload.resize(nPayload);
    for (std::uint64_t i = 0; i < nPayload; ++i) {
        if (!enterSubblock(r, CHOICE_PAYLOAD_BLOCK)) return false;
        if (!readChoicePayloadFieldBody(r, cc.payload[i])) return false;
    }
    return finishBlock(r);
}

// §9.8 tier 4 (#3179, family F10 of umbrella #3152): one entry of the
// `polymorphic("d": T, ...)` registry. Mirrors readChoiceCaseBody (zero
// scalars at the head — the `discriminator`/`type` pair fully identifies the
// entry, matching emitPolyCase) followed by the discriminator string and the
// registered MetaType.
bool readPolyCaseBody(Reader& r, MetaTypeDecl::MetaPolyCase& pc) {
    llvm::SmallVector<std::uint64_t, 1> sc;
    if (!expectScalars(r, sc)) return false;  // exactly 0 scalars allowed
    if (!expectString(r, pc.discriminator)) return false;
    if (!readType(r, pc.type)) return false;
    return finishBlock(r);
}

bool readContractBody(Reader& r, MetaContractRecord& c) {
    llvm::SmallVector<std::uint64_t, 3> sc;
    if (!expectScalars(r, sc) || sc.size() < 3) return false;
    c.kind = static_cast<MetaContractKind>(sc[0]);
    c.isStatic = sc[1] != 0;
    c.tier = static_cast<MetaContractTier>(sc[2]);
    if (!expectString(r, c.canonicalText)) return false;
    return finishBlock(r);
}

bool readContract(Reader& r, MetaContractRecord& c) {
    if (!enterSubblock(r, CONTRACT_BLOCK)) return false;
    return readContractBody(r, c);
}

// Forward declarations for mutual recursion (readMemberBody ↔ readTypeDeclBody).
bool readMember(Reader& r, MetaMember& m);
bool readTypeDecl(Reader& r, MetaTypeDecl& ty);
bool readTypeDeclBody(Reader& r, MetaTypeDecl& ty);

bool readMemberBody(Reader& r, MetaMember& m) {
    llvm::SmallVector<std::uint64_t, 8> sc;
    if (!expectScalars(r, sc) || sc.size() < 8) return false;
    m.kind = static_cast<MetaMemberKind>(sc[0]);
    m.visibility = static_cast<MetaVisibility>(sc[1]);
    m.isOperator = sc[2] != 0;
    m.returnOwned = sc[3] != 0;
    m.hasEnumValue = sc[4] != 0;
    m.enumValue = static_cast<std::int64_t>(sc[5]);
    std::uint64_t nGeneric = sc[6];
    std::uint64_t nParams = sc[7];
    std::uint64_t nContracts = sc.size() >= 9 ? sc[8] : 0;
    m.isPure = sc.size() >= 10 && sc[9] != 0;
    // fix #2400 (§23/§2.8.2/§2.8.3 rule 4, additive within v7): 11th scalar.
    // A pre-#2400 blob has at most 10 scalars; hasSetter defaults to false —
    // see the field's comment in ModuleMetadata.hpp for why "no setter" is
    // the safe default for an old, not-yet-recompiled dependency.
    m.hasSetter = sc.size() >= 11 && sc[10] != 0;
    // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4, additive within
    // v7): 12th scalar. A pre-#2415 blob has at most 11 scalars;
    // setterVisibility defaults to `Private` — the reject-conservative
    // direction (see the field's comment in ModuleMetadata.hpp) — rather
    // than `Public`, which would silently reopen the exact bypass this fix
    // closes for any dependency compiled before it.
    m.setterVisibility = sc.size() >= 12
        ? static_cast<MetaVisibility>(sc[11])
        : MetaVisibility::Private;
    // fix #2478 (§9.2/§22.8, additive within v7): 13th scalar. A pre-#2478
    // blob has at most 12 scalars; isAutoProperty defaults to `false` — the
    // SAME "not storage" behavior a stale dependency already exhibits today
    // (see the field's comment in ModuleMetadata.hpp).
    m.isAutoProperty = sc.size() >= 13 && sc[12] != 0;
    // fix #2461 (§22.3/§23, additive within v7): 14th scalar. A pre-#2461
    // blob has at most 13 scalars; hasGetter defaults to `true` — NOT `false`
    // like hasSetter above, because a false default would FALSE-REJECT a
    // legitimate `ref` argument to a read-write property from a stale
    // dependency (see the field's comment in ModuleMetadata.hpp for the full
    // rationale). `true` preserves the pre-#2461 behavior for a stale
    // dependency (write-only-as-ref stays the pre-existing linker error;
    // read-write-as-ref stays a clean accept).
    m.hasGetter = sc.size() < 14 || sc[13] != 0;
    // fix #2421 (§24/§2.8.2, additive within v7): 15th/16th scalars. A
    // pre-#2421 blob has at most 14 scalars; both default to `false` — the
    // classification a plain instance sync `event E: T;` (the common case)
    // carries, so a stale not-yet-recompiled dependency keeps its already-
    // broken pre-fix behavior (the imported event silently no-op'd before
    // #2421 regardless) rather than guessing static/async and risking an
    // itable/struct-layout mismatch (see the field comments in
    // ModuleMetadata.hpp).
    m.isEventStatic = sc.size() >= 15 && sc[14] != 0;
    m.isEventAsync = sc.size() >= 16 && sc[15] != 0;
    // fix #2516 (§9.2/§9.3/§2.8.3 rule 4, additive within v7): 17th scalar.
    // A pre-#2516 blob has at most 16 scalars; isPositional defaults to `false`
    // — see the field comment in ModuleMetadata.hpp.
    m.isPositional = sc.size() >= 17 && sc[16] != 0;
    // fix #3221 (§22.10/§9.8/§2.8.3 rule 4): 18th scalar. A pre-#3221 blob
    // has at most 17 scalars; posParamBodyRef defaults to `false` — see the
    // field comment in ModuleMetadata.hpp (this is exactly the silent-accept
    // direction the bug measured, so a stale dependency degrades to its own
    // pre-fix behavior rather than guessing).
    m.posParamBodyRef = sc.size() >= 18 && sc[17] != 0;
    if (!expectString(r, m.name)) return false;
    if (!readStringList(r, m.modifiers)) return false;
    if (!readType(r, m.type)) return false;
    m.genericParams.resize(nGeneric);
    for (std::uint64_t i = 0; i < nGeneric; ++i) {
        if (!readGeneric(r, m.genericParams[i])) return false;
    }
    m.params.resize(nParams);
    for (std::uint64_t i = 0; i < nParams; ++i) {
        if (!readParam(r, m.params[i])) return false;
    }
    m.contracts.resize(nContracts);
    for (std::uint64_t i = 0; i < nContracts; ++i) {
        if (!readContract(r, m.contracts[i])) return false;
    }
    // §22.12 const value (schema v2): the optional trailing REC_STRING. Read it
    // tolerantly — a v1 blob has none, so `m.valueText` stays empty.
    // §11.3 (schema v4, issue #502): for NestedType members, a TYPE_BLOCK
    // follows the valueText REC_STRING. We scan the trailing content manually
    // to capture both without leaving the block.
    if (m.kind == MetaMemberKind::NestedType) {
        // Scan trailing content: capture the first REC_STRING as valueText,
        // then enter any TYPE_BLOCK for the nested declaration.
        bool gotValue = false;
        bool gotNested = false;
        while (true) {
            unsigned id = 0;
            Next n = nextEntry(r, id);
            if (n == Next::EndBlock) {
                r.cursor.ReadBlockEnd();
                return true;
            }
            if (n == Next::Record) {
                llvm::SmallVector<std::uint64_t, 32> vals;
                auto got = r.cursor.readRecord(id, vals);
                if (!got) { llvm::consumeError(got.takeError()); return false; }
                if (got.get() == REC_STRING && !gotValue) {
                    m.valueText.clear();
                    m.valueText.reserve(vals.size());
                    for (std::uint64_t v : vals)
                        m.valueText.push_back(static_cast<char>(v & 0xFF));
                    gotValue = true;
                }
                continue;
            }
            if (n == Next::SubBlock) {
                if (id == TYPE_BLOCK && !gotNested) {
                    // Enter and read the nested type.
                    m.nestedType = std::make_unique<MetaTypeDecl>();
                    if (r.cursor.EnterSubBlock(TYPE_BLOCK)) return false;
                    // readTypeDeclBody reads the content + finishBlock.
                    if (!readTypeDeclBody(r, *m.nestedType)) return false;
                    gotNested = true;
                } else {
                    if (r.cursor.SkipBlock()) return false;
                }
                continue;
            }
            return false;  // Error
        }
    }
    // Non-NestedType: scan trailing REC_STRINGs.
    // - First REC_STRING → valueText  (schema v2+)
    // - Second REC_STRING → ctorBodyText for Constructor  (#662c schema v5)
    // A v4 or earlier blob has at most one trailing REC_STRING; the second
    // capture is simply never reached.
    {
        std::string* caps[2] = { &m.valueText, nullptr };
        if (m.kind == MetaMemberKind::Constructor)
            caps[1] = &m.ctorBodyText;
        // §14.1 / #783: a Method member's second trailing REC_STRING (when
        // present) is the generic method body source text.
        else if (m.kind == MetaMemberKind::Method)
            caps[1] = &m.methodBodyText;
        // §2.8.2 / #2351: a Field member's second trailing REC_STRING (when
        // present) is the static field's initializer EXPRESSION source text.
        else if (m.kind == MetaMemberKind::Field)
            caps[1] = &m.fieldInitText;
        // §14.1 / §23 / §2.8.2 / §2.8.3 rule 4 (#2893): a Property or
        // Indexer member's second trailing REC_STRING (when present) is
        // the generic-class property/indexer body source text. Absent
        // for a non-generic class (the existing ABI is correct), for an
        // Auto-bodied property/indexer (no user body — the #2478
        // isAutoProperty bit is enough), and for any pre-#2893 blob.
        else if (m.kind == MetaMemberKind::Property ||
                 m.kind == MetaMemberKind::Indexer)
            caps[1] = &m.propertyBodyText;
        // fix #3140 (§9.8 / §10): an EnumConstant member's second trailing
        // REC_STRING (when present) is its `serialized "<name>"` wire rename.
        // Absent for a constant with no clause and for any pre-#3140 blob, in
        // which case enumWireName stays empty and the DECLARED name is the wire
        // name — which is exactly §9.8's rule for an unrenamed constant, so a
        // stale dependency degrades to the correct answer rather than a guess.
        else if (m.kind == MetaMemberKind::EnumConstant)
            caps[1] = &m.enumWireName;
        int capIdx = 0;
        while (true) {
            unsigned id = 0;
            Next n = nextEntry(r, id);
            if (n == Next::EndBlock) {
                r.cursor.ReadBlockEnd();
                return true;
            }
            if (n == Next::Record) {
                llvm::SmallVector<std::uint64_t, 32> vals;
                auto got = r.cursor.readRecord(id, vals);
                if (!got) { llvm::consumeError(got.takeError()); return false; }
                if (got.get() == REC_STRING && capIdx < 2 && caps[capIdx]) {
                    caps[capIdx]->clear();
                    caps[capIdx]->reserve(vals.size());
                    for (std::uint64_t v : vals)
                        caps[capIdx]->push_back(static_cast<char>(v & 0xFF));
                    ++capIdx;
                    continue;
                }
                // fix #3140 (§9.8, family F2): the `serialized` clause, captured
                // BY CODE — so it is read whatever its position relative to the
                // trailing body-text strings above, and its absence (a member
                // with no clause, or any pre-#3140 blob) simply leaves the
                // fields at "no clause", which is what that dependency's own
                // compiler produced.
                if (got.get() == REC_SER_CLAUSE && vals.size() >= 2) {
                    m.serializedSkip     = vals[0] != 0;
                    m.serializedOptional = vals[1] != 0;
                    m.serializedWireName.clear();
                    m.serializedWireName.reserve(vals.size() - 2);
                    for (std::size_t vi = 2; vi < vals.size(); ++vi)
                        m.serializedWireName.push_back(
                            static_cast<char>(vals[vi] & 0xFF));
                    continue;
                }
                // fix #3202 (§22.10 / §22.5 / §2.8.3 rule 4, v0.254): the
                // positional primary-ctor parameter's default-value text,
                // captured BY CODE for the same reason REC_SER_CLAUSE /
                // REC_DISPOSE_VERDICT above are coded: a coded record is
                // self-describing and skipRecord()'d by every reader this
                // format has had, so a pre-#3202 reader never sees this and a
                // pre-#3202 BLOB simply leaves `posParamDefaultText` empty
                // — which is the "no default" reading the loader and codegen
                // default arm both treat as "required". Exactly the
                // dependency-direction safety the format's other additive
                // coded records hold.
                if (got.get() == REC_POSPARAM_DEFAULT) {
                    m.posParamDefaultText.clear();
                    m.posParamDefaultText.reserve(vals.size());
                    for (std::uint64_t v : vals)
                        m.posParamDefaultText.push_back(
                            static_cast<char>(v & 0xFF));
                }
                continue;
            }
            if (n == Next::SubBlock) {
                if (r.cursor.SkipBlock()) return false;
                continue;
            }
            return false;  // Error
        }
    }
}

bool readMember(Reader& r, MetaMember& m) {
    if (!enterSubblock(r, MEMBER_BLOCK)) return false;
    return readMemberBody(r, m);
}

bool readTypeDeclBody(Reader& r, MetaTypeDecl& ty) {
    llvm::SmallVector<std::uint64_t, 6> sc;
    if (!expectScalars(r, sc) || sc.size() < 5) return false;
    ty.kind = static_cast<MetaTypeDeclKind>(sc[0]);
    ty.visibility = static_cast<MetaVisibility>(sc[1]);
    std::uint64_t nGeneric = sc[2];
    std::uint64_t nBase = sc[3];
    std::uint64_t nMembers = sc[4];
    // §9.2 (schema v4, issue #501): 6th scalar = hasPositionalFields.
    // A v3 blob (read by v4 reader) has only 5 scalars → treat as false.
    if (sc.size() >= 6) ty.hasPositionalFields = sc[5] != 0;
    std::uint64_t nInvariants = sc.size() >= 7 ? sc[6] : 0;
    // #3139 (additive within v7): 8th scalar = the built-in-interface base
    // count. A blob written before this field has 7 scalars → zero.
    // #3139. ⚠ `sc[7]` means "nBuiltinIfaces" only because the 8th scalar is the
    // LAST positional entry and is emitted conditionally (see emitType). If a 9th
    // unconditional scalar is ever added, THIS LINE reads it as a count and then
    // tries to read that many METATYPE_BLOCKs — a silent misparse. Any new scalar
    // must be conditional on the 8th, or carry its own record code.
    std::uint64_t nBuiltinIfaces = sc.size() >= 8 ? sc[7] : 0;
    if (!expectString(r, ty.name)) return false;
    if (!readStringList(r, ty.modifiers)) return false;
    ty.genericParams.resize(nGeneric);
    for (std::uint64_t i = 0; i < nGeneric; ++i) {
        if (!readGeneric(r, ty.genericParams[i])) return false;
    }
    ty.baseTypes.resize(nBase);
    for (std::uint64_t i = 0; i < nBase; ++i) {
        if (!readType(r, ty.baseTypes[i])) return false;
    }
    ty.members.resize(nMembers);
    for (std::uint64_t i = 0; i < nMembers; ++i) {
        if (!readMember(r, ty.members[i])) return false;
    }
    ty.invariants.resize(nInvariants);
    for (std::uint64_t i = 0; i < nInvariants; ++i) {
        if (!readContract(r, ty.invariants[i])) return false;
    }
    // §9.8 / §18.5 / §21.4 (#3139): the built-in-interface bases, in the same
    // position the writer put them — after the invariants, before any trailing
    // record.
    ty.builtinInterfaceBases.resize(nBuiltinIfaces);
    for (std::uint64_t i = 0; i < nBuiltinIfaces; ++i) {
        if (!readType(r, ty.builtinInterfaceBases[i])) return false;
    }
    // §14 / #662c (schema v5): trailing REC_STRING = dtorBodyText for generic
    // classes.  A v4 reader's finishBlock already skips all trailing content,
    // so this is safe.  Capture the first trailing REC_STRING as dtorBodyText.
    //
    // §12 / §31.4 (#2950): REC_EXC_BASE = the built-in exception base name.
    // Captured by CODE, so it is read whether or not the type is generic and
    // whatever order the two records appear in — the layout fact must survive
    // for every class, and the dtorBodyText string only ever exists for a
    // generic one.
    //
    // §35.1 (#3144): the trailing CHOICE_CASE_BLOCKs, captured by SUBBLOCK ID
    // and in emission order (which is the tag order). They carry no count
    // scalar — see the ⚠ block in emitTypeDecl for why a 9th positional scalar
    // was not an option — so "absent" and "empty" are the same read, which is
    // correct: a pre-#3144 artifact and a non-choice both have no cases.
    // fix #3191 (§3.6 / §28.9 / §21.9 / §2.8.2, family M2 of #3152):
    // REC_DISPOSE_VERDICT = the §3.6 trivial-dispose verdict, inverted. Captured
    // by CODE like REC_EXC_BASE, so order relative to the other trailing records
    // is immaterial. Absent means false — an old artifact, or a trivial-dispose
    // type — which is the pre-fix reading and the safe one (see the field
    // comment: `false` must never mean "known trivial").
    //
    // §9.8 (#3179, family F10 of umbrella #3152): the SEC_TYPE_SHAPE carry
    // through the new `secondCodedCapture` slot, and serializedPolyCases
    // through POLY_CASE_BLOCK subblocks — both nullable for any other call
    // site, present only here. The byte ordering is immaterial because the
    // record is captured by code and the subblock is captured by id, exactly
    // matching the emission's self-describing layout. Absent on either is the
    // pre-fix reading: an empty `serializedShape` and an empty registry, both
    // of which a non-§9.8-class wants anyway.
    return finishBlockImpl(
        r, !ty.genericParams.empty() ? &ty.dtorBodyText : nullptr,
        REC_EXC_BASE, &ty.builtinExceptionBase, &ty.choiceCases,
        &ty.nonTrivialDispose,
        REC_SER_TYPE_SHAPE, &ty.serializedShape, &ty.serializedPolyCases);
}

bool readTypeDecl(Reader& r, MetaTypeDecl& ty) {
    if (!enterSubblock(r, TYPE_BLOCK)) return false;
    return readTypeDeclBody(r, ty);
}

bool readFunctionBody(Reader& r, MetaFunction& fn) {
    llvm::SmallVector<std::uint64_t, 4> sc;
    if (!expectScalars(r, sc) || sc.size() < 4) return false;
    fn.visibility = static_cast<MetaVisibility>(sc[0]);
    fn.returnOwned = sc[1] != 0;
    std::uint64_t nGeneric = sc[2];
    std::uint64_t nParams = sc[3];
    std::uint64_t nContracts = sc.size() >= 5 ? sc[4] : 0;
    fn.isPure = sc.size() >= 6 && sc[5] != 0;
    if (!expectString(r, fn.name)) return false;
    if (!readStringList(r, fn.modifiers)) return false;
    if (!readType(r, fn.returnType)) return false;
    fn.genericParams.resize(nGeneric);
    for (std::uint64_t i = 0; i < nGeneric; ++i) {
        if (!readGeneric(r, fn.genericParams[i])) return false;
    }
    fn.params.resize(nParams);
    for (std::uint64_t i = 0; i < nParams; ++i) {
        if (!readParam(r, fn.params[i])) return false;
    }
    fn.contracts.resize(nContracts);
    for (std::uint64_t i = 0; i < nContracts; ++i) {
        if (!readContract(r, fn.contracts[i])) return false;
    }
    return finishBlock(r);
}

bool readFunction(Reader& r, MetaFunction& fn) {
    if (!enterSubblock(r, FUNC_BLOCK)) return false;
    return readFunctionBody(r, fn);
}

bool readConstantBody(Reader& r, MetaConstant& c) {
    llvm::SmallVector<std::uint64_t, 1> sc;
    if (!expectScalars(r, sc) || sc.empty()) return false;
    c.visibility = static_cast<MetaVisibility>(sc[0]);
    if (!expectString(r, c.name)) return false;
    if (!expectString(r, c.valueText)) return false;
    if (!readType(r, c.type)) return false;
    return finishBlock(r);
}

bool readConstant(Reader& r, MetaConstant& c) {
    if (!enterSubblock(r, CONST_BLOCK)) return false;
    return readConstantBody(r, c);
}

// §26 (#735): the inverse of emitExtension.
bool readExtensionBody(Reader& r, MetaExtension& ext) {
    llvm::SmallVector<std::uint64_t, 7> sc;
    if (!expectScalars(r, sc) || sc.size() < 2) return false;
    ext.visibility = static_cast<MetaVisibility>(sc[0]);
    std::uint64_t nMembers = sc[1];
    // §26.5 rule 4 (#2329): absent on older blobs, where all fields retain
    // their reject-conservative/default diagnostic values.
    const bool hasDiagnosticMetadata = sc.size() >= 7;
    if (hasDiagnosticMetadata) {
        ext.sourceSpan.beginLine = static_cast<std::uint32_t>(sc[2]);
        ext.sourceSpan.beginColumn = static_cast<std::uint32_t>(sc[3]);
        ext.sourceSpan.endLine = static_cast<std::uint32_t>(sc[4]);
        ext.sourceSpan.endColumn = static_cast<std::uint32_t>(sc[5]);
    }
    const std::uint64_t nConstraints = hasDiagnosticMetadata ? sc[6] : 0;
    if (!readType(r, ext.target)) return false;
    ext.members.resize(nMembers);
    for (std::uint64_t i = 0; i < nMembers; ++i) {
        if (!readMember(r, ext.members[i])) return false;
    }
    if (hasDiagnosticMetadata) {
        if (!expectString(r, ext.sourceSpan.file)) return false;
        ext.genericConstraints.resize(nConstraints);
        for (std::uint64_t i = 0; i < nConstraints; ++i)
            if (!expectString(r, ext.genericConstraints[i])) return false;
    }
    return finishBlock(r);
}

bool readExtension(Reader& r, MetaExtension& ext) {
    if (!enterSubblock(r, EXTENSION_BLOCK)) return false;
    return readExtensionBody(r, ext);
}

// Validate magic + read the header record. Positions the cursor inside the
// MODULE_BLOCK, just after the header record. Reports the schema version + hash
// + producer content-revision stamp via out-params. Returns false on bad magic
// / newer schema / malformed input. The stamp is 0 on any artifact written
// before the field landed (hdr.size() < 5) — see #3198.
bool readHeader(Reader& r, std::uint32_t& version, std::uint64_t& hash,
                std::uint32_t& producerRevision) {
    // The bitstream has no 4-byte file magic of its own here (we don't write
    // the LLVM 'BC' wrapper); the MODULE_BLOCK's HEADER record carries our
    // magic. Enter the top-level MODULE_BLOCK first.
    if (!enterSubblock(r, MODULE_BLOCK)) return false;
    llvm::SmallVector<std::uint64_t, 5> hdr;
    // The header is the first record; skip-unknown still applies (a future
    // schema might prepend records, though current writers do not).
    while (true) {
        unsigned code = 0;
        if (!readNextRecord(r, code, hdr)) return false;
        if (code == REC_HEADER) break;
        // unknown leading record: keep looking (read-older / skip-unknown)
    }
    if (hdr.size() < 4) return false;
    if (hdr[0] != kMetadataMagic) return false;
    version = static_cast<std::uint32_t>(hdr[1]);
    // Read-older: a reader rejects a blob whose schema is NEWER than it knows
    // (it cannot guarantee correct interpretation). Equal/older is accepted.
    if (version > kSchemaVersion) return false;
    hash = (hdr[2] & 0xFFFFFFFFu) | ((hdr[3] & 0xFFFFFFFFu) << 32);
    // §2.5 / §2.8.2 (issue #3198, v0.254): the producer content-revision
    // stamp is the 5th header value (additive — no schema bump, an old reader
    // silently ignores it). Absent (hdr.size() < 5) reads as 0, which is the
    // sentinel for "stale artifact" — `kProducerContentRevision` is always >=
    // 1, so a stamp of 0 fails the loader's vintage check. The semantic hash
    // is folded AFTER the stamp (see semanticHash) so the hash covers the
    // stamp value too — a "stale hash" not only differs from the current
    // hash, but ALSO covers the stamp value embedded in the header.
    producerRevision = hdr.size() >= 5
        ? static_cast<std::uint32_t>(hdr[4])
        : 0u;
    return true;
}

} // namespace

std::optional<ModuleMetadata> deserialize(
    const std::vector<std::uint8_t>& bytes) {
    Reader r(bytes);
    std::uint32_t version = 0;
    std::uint64_t headerHash = 0;
    std::uint32_t producerRevision = 0;
    if (!readHeader(r, version, headerHash, producerRevision)) return std::nullopt;

    ModuleMetadata md;
    // Module name.
    if (!expectString(r, md.moduleName)) return std::nullopt;
    // Exports.
    {
        llvm::SmallVector<std::uint64_t, 1> ec;
        if (!expectScalars(r, ec) || ec.empty()) return std::nullopt;
        md.exports.resize(ec[0]);
        for (std::uint64_t i = 0; i < ec[0]; ++i) {
            if (!expectString(r, md.exports[i])) return std::nullopt;
        }
    }
    // Top-level counts.
    llvm::SmallVector<std::uint64_t, 4> counts;
    if (!expectScalars(r, counts) || counts.size() < 3) return std::nullopt;
    // §2.8.4 rule 4 (issue #1733, schema v6): the optional 4th scalar is the
    // artifact-kind flag. Absent (a v<=5 blob) → false = library-kind, so an
    // existing library artifact still loads as a library (back-compat).
    md.isExecutable = counts.size() >= 4 && counts[3] != 0;
    // §2.8.6 (#2054): a pre-v7 blob carries no contract claim. Current v7
    // blobs write an explicit fifth scalar even when the record vectors are
    // empty, so absence is distinguishable from an asserted empty set.
    md.contractRecordsCarried = counts.size() >= 5 && counts[4] != 0;
    md.types.resize(counts[0]);
    for (std::uint64_t i = 0; i < counts[0]; ++i) {
        if (!readTypeDecl(r, md.types[i])) return std::nullopt;
    }
    md.functions.resize(counts[1]);
    for (std::uint64_t i = 0; i < counts[1]; ++i) {
        if (!readFunction(r, md.functions[i])) return std::nullopt;
    }
    md.constants.resize(counts[2]);
    for (std::uint64_t i = 0; i < counts[2]; ++i) {
        if (!readConstant(r, md.constants[i])) return std::nullopt;
    }
    // §26 (#735): the optional 6th scalar is the exported-extension count.
    // Absent (every pre-#735 blob, and any module with no exported extension)
    // → 0, so nothing is read and the decode ends exactly as it did before.
    const std::uint64_t nExtensions = counts.size() >= 6 ? counts[5] : 0;
    md.extensions.resize(nExtensions);
    for (std::uint64_t i = 0; i < nExtensions; ++i) {
        if (!readExtension(r, md.extensions[i])) return std::nullopt;
    }
    return md;
}

std::optional<std::uint32_t> readSchemaVersion(
    const std::vector<std::uint8_t>& bytes) {
    Reader r(bytes);
    std::uint32_t version = 0;
    std::uint64_t hash = 0;
    std::uint32_t producerRevision = 0;
    if (!readHeader(r, version, hash, producerRevision)) return std::nullopt;
    return version;
}

std::optional<std::uint64_t> readSemanticHash(
    const std::vector<std::uint8_t>& bytes) {
    Reader r(bytes);
    std::uint32_t version = 0;
    std::uint64_t hash = 0;
    std::uint32_t producerRevision = 0;
    if (!readHeader(r, version, hash, producerRevision)) return std::nullopt;
    return hash;
}

// §2.5 / §2.8.2 (issue #3198, v0.254): the producer's content-revision stamp,
// read from the blob's header record without fully decoding the body. The
// loader compares it against `kProducerContentRevision` and rejects the
// dependency if they differ — the vintage check that the issue demands. A
// 0 return value means "stamp absent" (an artifact written before this field
// landed) — the loader's check is then "any 0 != kProducerContentRevision =>
// reject", which is the recompile-the-world migration we want.
std::optional<std::uint32_t> readProducerContentRevision(
    const std::vector<std::uint8_t>& bytes) {
    Reader r(bytes);
    std::uint32_t version = 0;
    std::uint64_t hash = 0;
    std::uint32_t producerRevision = 0;
    if (!readHeader(r, version, hash, producerRevision)) return std::nullopt;
    return producerRevision;
}

} // namespace bezel::module
