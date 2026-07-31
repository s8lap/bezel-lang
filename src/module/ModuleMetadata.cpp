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
    // 17.. reserved for future additive subblocks (a reader skips unknown ids)
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
    // 9.. additive; unknown codes are skipRecord()'d by the reader
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
      isPure(o.isPure), hasSetter(o.hasSetter),
      setterVisibility(o.setterVisibility), isAutoProperty(o.isAutoProperty),
      contracts(o.contracts),
      genericParams(o.genericParams), params(o.params),
      isOperator(o.isOperator), returnOwned(o.returnOwned),
      hasEnumValue(o.hasEnumValue), enumValue(o.enumValue) {
    if (o.nestedType)
        nestedType = std::make_unique<MetaTypeDecl>(*o.nestedType);
}

MetaMember& MetaMember::operator=(const MetaMember& o) {
    if (this != &o) {
        kind = o.kind; name = o.name; visibility = o.visibility;
        modifiers = o.modifiers; type = o.type; valueText = o.valueText;
        ctorBodyText = o.ctorBodyText; methodBodyText = o.methodBodyText;
        fieldInitText = o.fieldInitText;
        isPure = o.isPure; hasSetter = o.hasSetter;
        setterVisibility = o.setterVisibility;
        isAutoProperty = o.isAutoProperty; contracts = o.contracts;
        genericParams = o.genericParams; params = o.params;
        isOperator = o.isOperator; returnOwned = o.returnOwned;
        hasEnumValue = o.hasEnumValue; enumValue = o.enumValue;
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
           isAutoProperty == o.isAutoProperty && contracts == o.contracts &&
           genericParams == o.genericParams && params == o.params &&
           isOperator == o.isOperator && returnOwned == o.returnOwned &&
           hasEnumValue == o.hasEnumValue && enumValue == o.enumValue &&
           valueText == o.valueText && ctorBodyText == o.ctorBodyText &&
           methodBodyText == o.methodBodyText &&
           fieldInitText == o.fieldInitText &&
           nestedEq;
}

bool MetaTypeDecl::operator==(const MetaTypeDecl& o) const {
    return name == o.name && kind == o.kind && visibility == o.visibility &&
           modifiers == o.modifiers && genericParams == o.genericParams &&
           baseTypes == o.baseTypes && members == o.members &&
           invariants == o.invariants &&
           hasPositionalFields == o.hasPositionalFields &&
           dtorBodyText == o.dtorBodyText;
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
           members == o.members;
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
    llvm::SmallVector<std::uint64_t, 13> sc{
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
    llvm::SmallVector<std::uint64_t, 7> sc{
        static_cast<std::uint64_t>(ty.kind),
        static_cast<std::uint64_t>(ty.visibility),
        ty.genericParams.size(),
        ty.baseTypes.size(),
        ty.members.size(),
        ty.hasPositionalFields ? 1u : 0u,
        ty.invariants.size(),
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitString(w, ty.name);
    emitStringList(w, ty.modifiers);
    for (const MetaGenericParam& g : ty.genericParams) emitGeneric(w, g);
    for (const MetaType& b : ty.baseTypes) emitType(w, b);
    for (const MetaMember& m : ty.members) emitMember(w, m);
    for (const MetaContractRecord& c : ty.invariants) emitContract(w, c);
    // §14 / #662c (schema v5): for a GENERIC class, a trailing REC_STRING after
    // the members carries the destructor body source text (empty = no user body).
    // v4 readers encounter finishBlock which skips all trailing content, so this
    // is silently ignored (tolerant skip-unknown). Only emit for generic classes.
    if (!ty.genericParams.empty())
        emitString(w, ty.dtorBodyText);
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
    llvm::SmallVector<std::uint64_t, 2> sc{
        static_cast<std::uint64_t>(ext.visibility),
        ext.members.size(),
    };
    w.EmitRecord(REC_SCALARS, sc);
    emitType(w, ext.target);
    for (const MetaMember& m : ext.members) emitMember(w, m);
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
// reader's skip-unknown path (issue #328 forward-compat test).
std::vector<std::uint8_t> serializeImpl(const ModuleMetadata& md,
                                        bool withUnknownExtras) {
    llvm::SmallVector<char, 0> buffer;
    {
        llvm::BitstreamWriter w(buffer);
        w.EnterSubblock(MODULE_BLOCK, 4);

        // Header record first (issue #328: magic + schema version + hash).
        std::uint64_t hash = semanticHash(md);
        llvm::SmallVector<std::uint64_t, 4> hdr{
            kMetadataMagic, kSchemaVersion,
            hash & 0xFFFFFFFFu, (hash >> 32) & 0xFFFFFFFFu};
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
    return serializeImpl(md, /*withUnknownExtras=*/false);
}

std::vector<std::uint8_t> serializeWithUnknownExtras(const ModuleMetadata& md) {
    return serializeImpl(md, /*withUnknownExtras=*/true);
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
bool finishBlockImpl(Reader& r, std::string* captureString) {
    while (true) {
        unsigned id = 0;
        Next n = nextEntry(r, id);
        if (n == Next::EndBlock) {
            // AF_DontPopBlockAtEnd was used, so pop the scope now.
            if (r.cursor.ReadBlockEnd()) return false;
            return true;
        }
        if (n == Next::Record) {
            if (captureString != nullptr) {
                // Read this record; if it is a REC_STRING, capture it once.
                llvm::SmallVector<std::uint64_t, 32> vals;
                auto got = r.cursor.readRecord(id, vals);
                if (!got) { llvm::consumeError(got.takeError()); return false; }
                if (got.get() == REC_STRING) {
                    captureString->clear();
                    captureString->reserve(vals.size());
                    for (std::uint64_t v : vals)
                        captureString->push_back(static_cast<char>(v & 0xFF));
                    captureString = nullptr;  // capture only the first string
                }
                continue;
            }
            auto sk = r.cursor.skipRecord(id);
            if (!sk) { llvm::consumeError(sk.takeError()); return false; }
            continue;
        }
        if (n == Next::SubBlock) {
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
    // §14 / #662c (schema v5): trailing REC_STRING = dtorBodyText for generic
    // classes.  A v4 reader's finishBlock already skips all trailing content,
    // so this is safe.  Capture the first trailing REC_STRING as dtorBodyText.
    return finishBlockImpl(r, !ty.genericParams.empty() ? &ty.dtorBodyText
                                                        : nullptr);
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
    llvm::SmallVector<std::uint64_t, 2> sc;
    if (!expectScalars(r, sc) || sc.size() < 2) return false;
    ext.visibility = static_cast<MetaVisibility>(sc[0]);
    std::uint64_t nMembers = sc[1];
    if (!readType(r, ext.target)) return false;
    ext.members.resize(nMembers);
    for (std::uint64_t i = 0; i < nMembers; ++i) {
        if (!readMember(r, ext.members[i])) return false;
    }
    return finishBlock(r);
}

bool readExtension(Reader& r, MetaExtension& ext) {
    if (!enterSubblock(r, EXTENSION_BLOCK)) return false;
    return readExtensionBody(r, ext);
}

// Validate magic + read the header record. Positions the cursor inside the
// MODULE_BLOCK, just after the header record. Reports the schema version + hash
// via out-params. Returns false on bad magic / newer schema / malformed input.
bool readHeader(Reader& r, std::uint32_t& version, std::uint64_t& hash) {
    // The bitstream has no 4-byte file magic of its own here (we don't write
    // the LLVM 'BC' wrapper); the MODULE_BLOCK's HEADER record carries our
    // magic. Enter the top-level MODULE_BLOCK first.
    if (!enterSubblock(r, MODULE_BLOCK)) return false;
    llvm::SmallVector<std::uint64_t, 4> hdr;
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
    return true;
}

} // namespace

std::optional<ModuleMetadata> deserialize(
    const std::vector<std::uint8_t>& bytes) {
    Reader r(bytes);
    std::uint32_t version = 0;
    std::uint64_t headerHash = 0;
    if (!readHeader(r, version, headerHash)) return std::nullopt;

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
    if (!readHeader(r, version, hash)) return std::nullopt;
    return version;
}

std::optional<std::uint64_t> readSemanticHash(
    const std::vector<std::uint8_t>& bytes) {
    Reader r(bytes);
    std::uint32_t version = 0;
    std::uint64_t hash = 0;
    if (!readHeader(r, version, hash)) return std::nullopt;
    return hash;
}

} // namespace bezel::module
