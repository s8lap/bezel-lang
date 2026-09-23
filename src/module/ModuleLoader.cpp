#include "module/ModuleLoader.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lexer/Lexer.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"  // kMetadataSectionName
#include "common/IntLiteral.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

// Cross-module import loader (§2.2 / §2.5, issue #328 increment 3). The inverse
// of ModuleMetadataBuilder: an artifact's embedded `.bezel.meta` →
// ModuleMetadata → live imported sema symbols in the importer's context. See
// ModuleLoader.hpp for the design and the deferred-scope note.

namespace bezel::module {

// --- section extraction (LLVM Object API) -----------------------------------

std::optional<std::vector<std::uint8_t>> readMetadataSection(
    const std::string& artifactPath) {
    // Map the artifact and parse it as an object/executable. The metadata lives
    // in a non-allocated SHT_PROGBITS section the linker preserves by name; we
    // walk the section table and copy the one named `.bezel.meta`.
    auto bufOrErr = llvm::MemoryBuffer::getFile(artifactPath);
    if (!bufOrErr) return std::nullopt;
    auto binOrErr = llvm::object::createBinary((*bufOrErr)->getMemBufferRef());
    if (!binOrErr) {
        llvm::consumeError(binOrErr.takeError());
        return std::nullopt;
    }
    auto* obj = llvm::dyn_cast<llvm::object::ObjectFile>(binOrErr->get());
    if (!obj) return std::nullopt;

    // The section name carried in the object table drops the leading dot on
    // some formats but ELF (the Fedora toolchain target) keeps it. Match both
    // `.bezel.meta` and `bezel.meta` to be format-robust.
    const std::string wanted = kMetadataSectionName;          // ".bezel.meta"
    const std::string wantedNoDot =
        wanted.size() > 1 && wanted[0] == '.' ? wanted.substr(1) : wanted;

    for (const llvm::object::SectionRef& sec : obj->sections()) {
        auto nameOrErr = sec.getName();
        if (!nameOrErr) {
            llvm::consumeError(nameOrErr.takeError());
            continue;
        }
        llvm::StringRef name = *nameOrErr;
        if (name != wanted && name != wantedNoDot) continue;
        auto contentsOrErr = sec.getContents();
        if (!contentsOrErr) {
            llvm::consumeError(contentsOrErr.takeError());
            return std::nullopt;
        }
        llvm::StringRef data = *contentsOrErr;
        return std::vector<std::uint8_t>(data.bytes_begin(), data.bytes_end());
    }
    return std::nullopt;  // no `.bezel.meta` section
}

namespace {

// --- MetaType → TypeRef reconstruction --------------------------------------

sema::PrimitiveType fromPrimitiveKind(PrimitiveKind p) {
    switch (p) {
        case PrimitiveKind::None:    return sema::PrimitiveType::None;
        case PrimitiveKind::Bool:    return sema::PrimitiveType::Bool;
        case PrimitiveKind::Char:    return sema::PrimitiveType::Char;
        case PrimitiveKind::String:  return sema::PrimitiveType::String;
        case PrimitiveKind::Byte:    return sema::PrimitiveType::Byte;
        case PrimitiveKind::SByte:   return sema::PrimitiveType::SByte;
        case PrimitiveKind::Short:   return sema::PrimitiveType::Short;
        case PrimitiveKind::UShort:  return sema::PrimitiveType::UShort;
        case PrimitiveKind::Int:     return sema::PrimitiveType::Int;
        case PrimitiveKind::UInt:    return sema::PrimitiveType::UInt;
        case PrimitiveKind::Long:    return sema::PrimitiveType::Long;
        case PrimitiveKind::ULong:   return sema::PrimitiveType::ULong;
        case PrimitiveKind::Float:   return sema::PrimitiveType::Float;
        case PrimitiveKind::Double:  return sema::PrimitiveType::Double;
        case PrimitiveKind::Decimal: return sema::PrimitiveType::Decimal;
        case PrimitiveKind::Object:  return sema::PrimitiveType::Object;
    }
    return sema::PrimitiveType::None;
}

// fix #3230 (§17.14 / §15.1 / §22.12; family M11 of umbrella #3152): parse
// the textual representation of an IMPORTED integer const's compile-time
// value into a 64-bit signed integer. The wire carries a SINGLE integer
// literal — `MetaMember::valueText` for an integer-typed const is the FOLDED
// value (a digit string, possibly signed — fix #2898's "FOLD, don't render"),
// not the source lexeme — so we parse with the same sign-split logic as
// codegen's foldImportedConstText (LlvmBackend_Setup.cpp) and the same
// magnitude decoder as `parseIntLiteral` (common/IntLiteral.hpp, the one
// decoder for an A.8 literal — `0x` / `0b` / decimal / `_` separators all
// admitted). Returns nullopt when the text does not start with a digit/sign
// (a non-integer const reference, a reference to another const/enum by
// name, an unparseable shape) — sema's fold closures then fall back to
// returning nullopt, exactly the pre-#3230 behavior. The schema's wire
// shape stays unchanged: the existing `valueText` is the only carrier and
// we parse it at load time rather than on every codegen/materialize site.
std::optional<std::int64_t> parseImportedConstIntText(std::string_view text) {
    if (text.empty()) return std::nullopt;
    const char c0 = text.front();
    // Only fold an integer LITERAL (sign + magnitude). §22.12 permits a const
    // initializer to reference another const/enum, which renderConstExpr
    // emitted BY NAME (not as a digit string); folding that as an integer
    // would silently produce 0. Reject it here so the caller's nullopt arm
    // is the same fall-back it always was. The sign character is admitted
    // because the textual channel already admits it (the guard mirrors
    // LlvmBackend_Setup.cpp:551-552 one-for-one).
    if (!((c0 >= '0' && c0 <= '9') || c0 == '+' || c0 == '-')) return std::nullopt;
    const bool negative = (c0 == '-');
    std::string_view mag = text;
    if (c0 == '-' || c0 == '+') mag.remove_prefix(1);
    // parseIntLiteral returns the unsigned magnitude as an i64 bit pattern;
    // long.MinValue's magnitude is 2^63, which has no positive i64 — so
    // negate in the UNSIGNED domain (~mv + 1u, two's complement with no
    // signed arithmetic; same shape as LlvmBackend_Setup.cpp:574-583).
    const std::uint64_t mv =
        static_cast<std::uint64_t>(common::parseIntLiteral(mag));
    return negative ? static_cast<std::int64_t>(~mv + 1u)
                    : static_cast<std::int64_t>(mv);
}

ast::TypeDeclKind fromMetaTypeDeclKind(MetaTypeDeclKind k) {
    switch (k) {
        case MetaTypeDeclKind::Class:     return ast::TypeDeclKind::Class;
        case MetaTypeDeclKind::Interface: return ast::TypeDeclKind::Interface;
        case MetaTypeDeclKind::Record:    return ast::TypeDeclKind::Record;
        case MetaTypeDeclKind::Enum:      return ast::TypeDeclKind::Enum;
        // §35 (v0.200, #2044, #2121): mirror the toMetaTypeDeclKind arm; the
        // loaded TypeSymbol carries the Choice kind, so the importer's
        // downstream classifiers (which all branch on ts->kind) take the
        // choice path and not the Class fallback.
        case MetaTypeDeclKind::Choice:    return ast::TypeDeclKind::Choice;
    }
    return ast::TypeDeclKind::Class;
}

ast::Visibility fromMetaVisibility(MetaVisibility v) {
    switch (v) {
        case MetaVisibility::None:      return ast::Visibility::None;
        case MetaVisibility::Public:    return ast::Visibility::Public;
        case MetaVisibility::Private:   return ast::Visibility::Private;
        case MetaVisibility::Internal:  return ast::Visibility::Internal;
        case MetaVisibility::Protected: return ast::Visibility::Protected;
    }
    return ast::Visibility::None;
}

std::vector<sema::ContractRecord> toContractRecords(
    const std::vector<MetaContractRecord>& in) {
    std::vector<sema::ContractRecord> out;
    out.reserve(in.size());
    for (const MetaContractRecord& mc : in) {
        sema::ContractRecord c;
        c.kind = static_cast<sema::ContractKind>(mc.kind);
        c.isStatic = mc.isStatic;
        c.canonicalText = mc.canonicalText;
        c.tier = static_cast<sema::ContractTier>(mc.tier);
        out.push_back(std::move(c));
    }
    return out;
}

// fix #4508 (§15.1 / §2.8.3 rule 4, v0.306): the ONE decode of an inline array's
// EXTENT from its textual form, shared by every reader that needs the number.
//
// `MetaType::arraySize` carries the extent as a folded decimal (#3183); the
// synthetic reconstruction source `metaTypeToSourceText` writes carries the same
// digits; and `Loader::convert` turns them into `TypeRef::arraySizeValue`. Three
// readers, one rule — and two of the three are the §14/#705 signature-key
// renderers that must agree BYTE FOR BYTE (`metaTypeSigText` vs
// `astTypeToSigText`), so a second copy of the rule is a silent
// stops-matching defect waiting to happen. Returns the extent, or -1 for any
// shape that is not a well-formed positive decimal — including the empty string
// an old artifact carries and the deliberate omission for a size the exporter
// could not fold, which #3183 keeps failing loudly downstream.
static long long metaInlineArrayExtent(std::string_view s) {
    if (s.empty() || s.size() > 18) return -1;  // 18 digits < 2^63, no overflow
    long long n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n > 0 ? n : -1;
}

// One reconstruction context for a single loadMetadata call. Holds the
// name → reconstructed-TypeSymbol map so a MetaType::Named referring to another
// exported type of the same module resolves to its symbol (§2.5 by-name
// resolution within the loaded set).
struct Loader {
    sema::SemaContext& ctx;
    // §2.5: resolve a Named MetaType's `namedName` against the types reconstructed
    // in THIS module first, then any type already loaded into the context (so a
    // cross-module reference between two loaded dependencies still binds).
    std::unordered_map<std::string, sema::TypeSymbol*>& localTypes;

    // fix #3540 (§11.6 / §2.8.3 rule 3 / §2.8.2, v0.285): Named references
    // whose `lookupNamed` came back null at conversion time, retried once by
    // `resolvePendingNamed` after every type of the module — including every
    // §11.6 NESTED type, which only comes into existence while pass 2 walks
    // its enclosing type's member list — has been registered in `localTypes`.
    std::vector<std::pair<sema::TypeRef*, std::string>> pendingNamed;

    sema::TypeRef* convert(const MetaType& mt) {
        auto* ref = ctx.makeImportedTypeRef();
        ref->nullable = mt.nullable;
        switch (mt.kind) {
            case TypeKind::Error:
                ref->kind = sema::TypeRef::Kind::Error;
                break;
            case TypeKind::Unit:
                ref->kind = sema::TypeRef::Kind::Unit;
                break;
            case TypeKind::Primitive:
                ref->kind = sema::TypeRef::Kind::Primitive;
                ref->primitive = fromPrimitiveKind(mt.primitive);
                break;
            case TypeKind::Named: {
                ref->kind = sema::TypeRef::Kind::Named;
                ref->declaration = lookupNamed(mt.namedName);
                // §2.5 / §29.3 (issue #495): when the named type is a builtin
                // interface or stdlib class (IEnumerable<T>, Array<T>, Task,
                // etc.) it is NOT an exported user type in the dependency, so
                // lookupNamed returns null.  The resulting TypeRef then has
                // declaration=nullptr AND astNode=nullptr (no AST exists for an
                // imported type), so lowerTypeRef cannot read the type's name
                // from either field and falls through to the i32 default — which
                // truncates pointer-sized types, causing SIGSEGV when the
                // vtable-backed iterator pointer is stored in a 32-bit slot
                // (bug A) or SIGABRT from the bad load type (bug B).
                //
                // Fix: record the name in importedName so lowerTypeRef can
                // read it via the `importedName` path (the dedicated imported-
                // type name branch, independent of astNode/declaration).
                if (ref->declaration == nullptr && !mt.namedName.empty())
                    ref->importedName = ctx.internString(mt.namedName);
                // fix #2776 (spec §14.2 rule 3, §2.5, v0.240): give an IMPORTED
                // occurrence of a synthesized builtin interface the SAME
                // TypeSymbol identity a local occurrence gets.
                //
                // `lookupNamed` searches the dependency's exported user types,
                // and `IComparable` is not one — it is compiler-synthesized —
                // so the base recorded for an imported `interface IRank:
                // IComparable<IRank>` came back with `declaration == nullptr`.
                // MEASURED with a two-module build (`bezelc -c lib.bzl` then
                // `--import lib.o app.bzl`): the base's head is `IComparable`
                // but its declaration is nil while the local builtin symbol is
                // a real pointer, so #1867's IDENTITY test — which
                // TypeResolution_Checks.cpp's selfComparableIface applies, and
                // which exists because "blanket-accepting by decl/name was too
                // lenient" — could never match. The consequence was that
                // `Array<IRank>.Sort()/.Min()/.Max()` and `Math.Min` all
                // silently declined a document-admissible CROSS-MODULE element,
                // the limit recorded on #2776 and noted in
                // selfComparableIfaceCompareTo's header.
                //
                // Resolving to `builtinInterfaceSym` here fixes it once for
                // EVERY consumer of that identity rather than per call site —
                // which is the point of #2787 having collapsed the predicate to
                // a single definition. The name set is
                // SemaContext::isSynthesizedBuiltinInterfaceName, the same
                // closed list the local name-resolution site uses, so the two
                // cannot drift.
                if (ref->declaration == nullptr && !mt.namedName.empty() &&
                    sema::SemaContext::isSynthesizedBuiltinInterfaceName(
                        mt.namedName))
                    ref->declaration = ctx.builtinInterfaceSym(mt.namedName);
                // fix #3540 (§11.6 / §2.8.3 rule 3, v0.285): still unbound.
                // It may simply be a name this loader can never resolve (a
                // builtin like `Task` or `Array<T>` has no exported user type,
                // which is what the `importedName` carriage above is for) — or
                // it may be a type of THIS module that pass 2 has not created
                // yet, which is the whole nested-type population. Record it and
                // retry once at the end of pass 2; see `resolvePendingNamed`.
                if (ref->declaration == nullptr && !mt.namedName.empty())
                    pendingNamed.emplace_back(ref, mt.namedName);
                for (const MetaType& ga : mt.genericArgs)
                    ref->genericArgs.push_back(convert(ga));
                break;
            }
            case TypeKind::InlineArray: {
                ref->kind = sema::TypeRef::Kind::InlineArray;
                ref->arrayOwned = mt.arrayOwned;
                if (!mt.children.empty())
                    ref->arrayElement = convert(mt.children.front());
                // fix #3183 (§15.1 / §2.8.3 rule 4, v0.254): restore N.
                //
                // This used to read "arraySize (a const expr) is not
                // reconstructed … an imported inline-array field/return is a
                // declaration the importer can name but not yet lower" — and
                // "not yet lower" was the optimistic reading: `lowerTypeRef`
                // did not refuse, it returned a bare `i32` PLACEHOLDER, so an
                // imported `[3]int` component was laid out in 4 bytes against
                // the exporter's 12 and constructing the type was a measured
                // stack-buffer-overflow (see the builder's comment).
                //
                // `ModuleMetadataBuilder` now writes the FOLDED size as
                // decimal, so nothing is re-evaluated here: no const-expr
                // evaluator, no importer-side scope, no identifiers. Only a
                // well-formed positive decimal is accepted; anything else
                // (including the empty string an OLD artifact carries, and the
                // deliberate omission for a size the exporter could not fold)
                // leaves `arraySizeValue` at -1, which is the condition
                // `sema::serializationComponentTypeIsEmittable` and
                // `Emitter::lowerTypeRef` both key on to fail LOUDLY.
                //
                // fix #4508: the acceptance rule moved VERBATIM into
                // `metaInlineArrayExtent` (semantically identical — a
                // well-formed positive decimal of at most 18 digits, -1
                // otherwise, which is already this field's default) so the two
                // §14/#705 signature-key renderers decode the same text the
                // same way. See that helper's header.
                ref->arraySizeValue = metaInlineArrayExtent(mt.arraySize);
                break;
            }
            case TypeKind::Function: {
                ref->kind = sema::TypeRef::Kind::Function;
                ref->funcReturnOwned = mt.funcReturnOwned;
                // children = params… then return.
                const std::size_t n =
                    mt.children.empty() ? 0 : mt.children.size() - 1;
                for (std::size_t i = 0; i < n; ++i) {
                    sema::TypeRef::FuncParam fp;
                    if (i < mt.funcParamMods.size())
                        fp.modifier = ctx.internString(mt.funcParamMods[i]);
                    if (i < mt.funcParamNames.size())
                        fp.name = ctx.internString(mt.funcParamNames[i]);
                    fp.type = convert(mt.children[i]);
                    ref->funcParams.push_back(std::move(fp));
                }
                if (!mt.children.empty())
                    ref->funcReturn = convert(mt.children.back());
                break;
            }
        }
        return ref;
    }

    sema::TypeSymbol* lookupNamed(const std::string& name) {
        if (name.empty()) return nullptr;
        // Local types (the current module's own exported types) take priority.
        auto it = localTypes.find(name);
        if (it != localTypes.end()) return it->second;
        // §2.5 / §12 / §11.3 (issue #1223): fall back to types already loaded
        // from OTHER modules (e.g. a base class from a dependency when
        // reconstructing a cross-module `Extended: Base` inheritance chain).
        // Without this, `Extended.baseTypes` stays empty → lookupMember cannot
        // walk up to `Base`'s members (BAD_REJECT on GetX()) AND
        // collectInstanceStorage misses the base-subobject prefix → wrong
        // field offsets (Sum() returns 400 instead of 300).
        return ctx.lookupImportedTypeByName(name);
    }

    // fix #3540 (§11.6 / §2.8.3 rule 3 / §2.8.2, v0.285): retry every Named
    // reference `lookupNamed` could not bind while pass 2 was still running.
    //
    // `loadMetadataPass1` registers `md.types` — this module's TOP-LEVEL types
    // — and nothing else. A §11.6 NESTED type's TypeSymbol is not created until
    // pass 2 reaches its enclosing type's `MetaMemberKind::NestedType` member
    // and calls `reconstructTypeSymbol`, so every signature converted BEFORE
    // that point saw a null for it: a sibling nested type declared later in the
    // same `Outer`, or any member of a top-level type that precedes `Outer` in
    // `md.types`. `reconstructTypeSymbol` registers a nested type before
    // walking its own members, which covers the self-reference of the issue's
    // repro; this pass covers the rest, and makes the result independent of
    // declaration order in the same way #1487 made the top-level pass
    // order-independent.
    //
    // The retry can only ever turn a null into a symbol. A reference that stays
    // unresolved keeps EXACTLY its pre-#3540 shape, `importedName` included, so
    // the builtin population (`Task`, `Array<T>`, `IEnumerable<T>`, …) is
    // untouched. On a successful retry `importedName` is cleared, so the
    // TypeRef becomes indistinguishable from one `lookupNamed` bound eagerly —
    // every reader that prefers `declaration` and falls back to `importedName`
    // (`typeRefSigText`, `TypeRef::headName`, codegen's name probes) then sees
    // one consistent state rather than a resolved symbol shadowed by a stale
    // name.
    void resolvePendingNamed() {
        for (auto& entry : pendingNamed) {
            sema::TypeRef* ref = entry.first;
            if (ref->declaration != nullptr) continue;
            if (sema::TypeSymbol* ts = lookupNamed(entry.second)) {
                ref->declaration = ts;
                ref->importedName = std::string_view{};
            }
        }
        pendingNamed.clear();
    }

    std::vector<std::string_view> intern(const std::vector<std::string>& in) {
        std::vector<std::string_view> out;
        out.reserve(in.size());
        for (const std::string& s : in) out.push_back(ctx.internString(s));
        return out;
    }

    std::vector<sema::Symbol*> convertParams(const std::vector<MetaParam>& in,
                                             sema::ScopeId scope) {
        std::vector<sema::Symbol*> out;
        out.reserve(in.size());
        for (const MetaParam& mp : in) {
            auto* p = ctx.makeImportedSymbol<sema::ParameterSymbol>();
            p->name = ctx.internString(mp.name);
            p->modifier = ctx.internString(mp.modifier);
            p->scope = scope;
            // The imported parameter carries an already-resolved type (there is
            // no syntactic ast::Type to attach), via resolvedDeclType — the
            // field identifier resolution / codegen reads when declType is null.
            p->resolvedDeclType = convert(mp.type);
            // §22.4 / §12.1 (#2392): reconstruct the `when true`/`when false`
            // out-nullability postcondition from MetaParam::outNullWhen, the
            // same way resolvedDeclType is reconstructed just above. Without
            // this, an imported base parameter's outNullPostcond stayed stuck
            // at its default None regardless of what the exporting module
            // declared, so checkOverrideSignatureMatch's exact-match
            // comparison (§12.1) false-rejected any correct override that
            // faithfully repeated the base's `when` clause.
            p->outNullPostcond =
                static_cast<ast::OutNullWhen>(mp.outNullWhen);
            // fix #3178 (§22.5 "Optional parameters" / §2.5, v0.254 — family M5
            // of umbrella #3152): `MetaParam::hasDefault` / `::defaultText` have
            // been written by ModuleMetadataBuilder and read back by the decoder
            // since the REC_SCALARS record, and were dropped HERE — the loader
            // simply never copied them onto the symbol. With no `hasDefault` on
            // the ParameterSymbol, every symbol-side arity check across a module
            // boundary had to demand EXACT arity, so an imported method's or
            // constructor's optional parameter could not be omitted: a measured
            // false reject of a legal program whose single-module twin compiles
            // and runs. No metadata format change was needed.
            p->hasDefault = mp.hasDefault;
            p->defaultText = ctx.internString(mp.defaultText);
            out.push_back(p);
        }
        return out;
    }

    std::vector<sema::Symbol*> convertGenericParams(
        const std::vector<MetaGenericParam>& in, sema::ScopeId scope) {
        std::vector<sema::Symbol*> out;
        out.reserve(in.size());
        for (const MetaGenericParam& mg : in) {
            auto* g = ctx.makeImportedSymbol<sema::GenericParamSymbol>();
            g->name = ctx.internString(mg.name);
            g->variance = ctx.internString(mg.variance);
            g->scope = scope;
            // fix #3146 (§14.2 / §2.8.3 rule 4, v0.252 — family F13 of umbrella
            // #3152): the constraint payload the metadata builder has exported
            // on every MetaGenericParam since #1579/v0.182 was read off the
            // wire HERE and thrown away, because GenericParamSymbol carried
            // only name/variance. `ast::TypeDecl::genericConstraints` was then
            // the only source, and it is null for every imported type — so
            // EVERY §14.2 constraint on EVERY imported generic type went
            // unchecked at every instantiation site, silently.
            //
            // MEASURED on 558c27c with a real two-module build: an exported
            // `class BoxC<T> where T: class` instantiated `BoxC<int>` in the
            // importer compiled clean, while the identical declarations in one
            // module reported "type argument `int` does not satisfy the
            // constraint `where T: class` (§14.2)".
            for (const std::string& sc : mg.specialConstraints) {
                // §14.4: the builder folds the historical `where T is owned` /
                // `is not owned` modifier form into this same list. It carries
                // no instantiation-time obligation, and the LOCAL populator
                // (SymbolCollection::collectGenericParams) drops it, so drop it
                // here too or the two sides would not agree.
                if (sc == "owned" || sc == "not owned") continue;
                sema::GenericParamConstraint c;
                c.kind = ctx.internString(sc);
                g->constraints.push_back(c);
            }
            for (const MetaType& ct : mg.constraintTypes) {
                sema::GenericParamConstraint c;
                c.kind = "type";  // the ast::ConstraintItem kind for a bound
                c.importedBound = convert(ct);
                g->constraints.push_back(c);
            }
            out.push_back(g);
        }
        return out;
    }

    // fix #3144 (§35.1 / §35.3 / §35.4, family F9 of umbrella #3152): rebuild
    // the choice's CASES as `ChoiceCaseSymbol`s pointing at synthetic
    // `ast::ChoiceCase` nodes — the #2516 `posParam` pattern, applied to the
    // one fact a choice cannot be laid out, classified, matched or disposed
    // without.
    //
    // WHY a reconstructed node rather than a sixth loader side table: a case's
    // payload is read through `ChoiceCaseSymbol::caseDecl` at 91 sites across
    // sema, ownership and codegen — layout (`choiceCasePayloadStruct`),
    // §35.4 classification (`classifyChoiceCore`), §35.5 matching, §35.3
    // payload binding, §28.1 owned-marker erasure, scope-end disposal. A side
    // table would fix whichever consumer motivated it and leave the other
    // ninety reading an empty list, which is precisely the failure the five
    // existing loader side tables each repeated. Pointing the imported symbol
    // at a faithful node makes the imported and local answers come from ONE
    // predicate, which is the audit's own rule for this seam.
    //
    // The reconstruction is FAITHFUL, not partial: name, payload field names,
    // the §28.1 `owned` marker, each payload's resolved type (published via
    // `setResolvedType`, so `ctx.resolvedType(*pf.type)` answers), and the §9.8
    // `serialized` clauses on both the case and its fields. A field omitted
    // here would not degrade — it would make every consumer confidently answer
    // the declaration's opposite, which is the defect this fixes.
    void convertChoiceCases(const MetaTypeDecl& td, sema::TypeSymbol& t) {
        for (const MetaChoiceCase& mc : td.choiceCases) {
            ast::ChoiceCase* cd = ctx.makeImportedChoiceCase();
            cd->name = ctx.internString(mc.name);
            if (mc.hasSerialized) {
                cd->serialized = std::make_unique<ast::SerializedClause>();
                cd->serialized->name = ctx.internString(mc.serializedName);
            }
            for (const MetaChoicePayloadField& mp : mc.payload) {
                ast::Param p;
                p.name = ctx.internString(mp.name);
                p.modifier = ctx.internString(mp.modifier);  // "" / "owned"
                p.type = makePayloadTypeNode(mp.type);
                if (mp.hasSerialized) {
                    p.serialized = std::make_unique<ast::SerializedClause>();
                    p.serialized->isSkip = mp.serializedSkip;
                    p.serialized->isOptional = mp.serializedOptional;
                    p.serialized->name = ctx.internString(mp.serializedName);
                }
                // §35.7 / §9.2 "The `equality skip` clause" (v0.307, #4652;
                // follow-up to fix #4643/#3152 family F9): reconstruct the
                // SAME `ast::EqualityClause` shape the parser attaches to a
                // local `ast::Param` (Parser_Decls.cpp's `parseCasePayloadList`),
                // so every consumer that reads `pf.equality` off a
                // `ChoiceCaseSymbol::caseDecl` payload — this function's own
                // header comment explains why `caseDecl` is a FAITHFUL
                // reconstruction rather than a sixth loader side table —
                // answers identically for a local and an imported case. A
                // field omitted here would repeat exactly the #4643 record-
                // side defect (0892d092): the clause invisible across the
                // module boundary while very much in effect inside the
                // declaring module.
                if (mp.equalitySkip) {
                    p.equality = std::make_unique<ast::EqualityClause>();
                    p.equality->isSkip = true;
                }
                cd->payload.push_back(std::move(p));
            }
            auto* cs = ctx.makeImportedSymbol<sema::ChoiceCaseSymbol>();
            cs->name = ctx.internString(mc.name);
            cs->parent = &t;
            cs->caseDecl = cd;
            cs->scope = t.memberScope;
            cs->declLoc = t.declLoc;
            // Declaration order IS the tag value, and `choiceCasesOf` reads
            // `members` in order — so appending in `td.choiceCases` order is
            // what makes the importer's tags agree with the exporter's.
            t.members.push_back(cs);
        }
    }

    // fix #3144: the synthetic `ast::Type` node a reconstructed payload field
    // points at. Consumers reach a payload's type ONLY through
    // `ctx.resolvedType(*pf.type)` (codegen's `lowerType(const ast::Type*)` is
    // itself just that lookup), so the node's job is to be a stable KEY with a
    // resolved entry; the syntactic head is filled in for diagnostics and for
    // `TypeRef::headName`'s astNode leg.
    std::unique_ptr<ast::Type> makePayloadTypeNode(const MetaType& mt) {
        auto node = std::make_unique<ast::Type>();
        node->nullable = mt.nullable;
        switch (mt.kind) {
            case TypeKind::Unit: node->kind = ast::TypeKind::Unit; break;
            case TypeKind::InlineArray:
                node->kind = ast::TypeKind::InlineArray;
                node->arrayOwned = mt.arrayOwned;
                break;
            case TypeKind::Function: node->kind = ast::TypeKind::Function; break;
            default: node->kind = ast::TypeKind::Named; break;
        }
        if (node->kind == ast::TypeKind::Named && !mt.namedName.empty()) {
            node->name = std::make_unique<ast::QualifiedName>();
            // fix #3540 (§11.6, v0.285): `namedName` is the QUALIFIED type
            // name, so a nested type arrives in its §11.6 surface spelling
            // ("Outer.Box" — "A nested type is referenced from outside as
            // `Outer.Nested`"), which is a MULTI-part QualifiedName. Pushing it
            // whole would make `parts[0]` the string "Outer.Box", an
            // identifier no lookup can match. A top-level name has no '.' and
            // so still produces exactly one part, unchanged.
            std::size_t pos = 0;
            for (;;) {
                const std::size_t dot = mt.namedName.find('.', pos);
                const std::size_t end =
                    dot == std::string::npos ? mt.namedName.size() : dot;
                node->name->parts.push_back(
                    ctx.internString(mt.namedName.substr(pos, end - pos)));
                if (dot == std::string::npos) break;
                pos = dot + 1;
            }
        }
        ctx.setResolvedType(node.get(), convert(mt));
        return node;
    }

    // Reconstruct a full TypeSymbol from a MetaTypeDecl (used for nested types,
    // §11.3 / issue #502). The reconstructed TypeSymbol lives in `parent`'s
    // member scope and has its own fresh member scope for its own members.
    sema::TypeSymbol* reconstructTypeSymbol(const MetaTypeDecl& td,
                                            sema::TypeSymbol& parent) {
        // fix #3581 (§11.6 / §2.8.3 rule 3 / §2.8.2, v0.286): reuse the
        // pass-1 shell created by `registerNestedTypeShells` so any
        // TypeRef whose `declaration` was bound to the shell (during a
        // CROSS-module consumer's pass-2 convert, or during a
        // SAME-module earlier-sibling pass-2 convert, where the lookup
        // found the shell in `importedByModule_` / `localTypes`
        // respectively) keeps pointing at the same address after the
        // upgrade. Creating a NEW symbol here would orphan the
        // TypeRef's `declaration` on a now-unreachable shell object
        // whose `qualifiedTypeName()` (correct) is the only thing the
        // metadata reader has — codegen would then read the SHELL's
        // empty `members` / `baseTypes` and treat the type as i32 (the
        // silent truncation #495 records).
        //
        // We look up the shell by its QUALIFIED name in BOTH tables —
        // `localTypes` for the SAME-module walk (this loader's
        // `registerNestedTypeShells` wrote the same module's nested
        // shells there), and `importedByModule_` as a backstop in case
        // a future caller doesn't pre-register locally.
        const std::string_view tdName = ctx.internString(td.name);
        std::string qn;
        qn.reserve(parent.qualifiedTypeName().size() + 1 + tdName.size());
        qn = parent.qualifiedTypeName();
        if (!qn.empty()) qn += '.';
        qn += std::string(tdName);
        sema::TypeSymbol* t = nullptr;
        if (auto it = localTypes.find(qn); it != localTypes.end() && it->second)
            t = it->second;
        if (!t)
            t = ctx.lookupImportedTypeByName(qn);
        sema::TypeSymbol* fresh = nullptr;
        if (!t) {
            // Defensive: no shell was pre-registered (e.g. the recursive
            // walker was bypassed). Create one so the rest of the path
            // remains well-defined; this is the pre-#3581 shape and
            // works as long as no cross-module consumer has already
            // bound a ref to a SHELL with a different address (it
            // hasn't, because there was no shell to bind to).
            fresh = ctx.makeImportedSymbol<sema::TypeSymbol>();
            t = fresh;
            t->name = tdName;
            t->kind = fromMetaTypeDeclKind(td.kind);
            t->visibility = fromMetaVisibility(td.visibility);
            t->modifiers = intern(td.modifiers);
            t->scope = parent.memberScope;
            t->memberScope = ctx.newScope();
            t->moduleName = parent.moduleName;
            t->enclosingTypePath = parent.qualifiedTypeName();
            localTypes[t->qualifiedTypeName()] = t;
            ctx.registerImportedType(parent.moduleName, t);
        }
        // §9.2 (issue #501): propagate positional-record flag.
        t->hasPositionalFields = td.hasPositionalFields;
        // fix #2950 (§12 / §31.4 / §2.8.3 rule 4, v0.240): restore the built-in
        // exception base. Without it this reconstructed symbol answers "no
        // built-in base" to every layout consumer and the §31.4 header is
        // dropped on the importing side only.
        t->builtinExceptionBase = td.builtinExceptionBase;
        // fix #3191 (§3.6 / §28.9 / §21.9 / §2.8.2, family M2 of #3152): replay
        // the exporter's §3.6 trivial-dispose verdict. Without it this
        // reconstructed symbol has no `DestructorSymbol` (the destructor is not
        // an exported member, §13.3) and so answers "trivial-dispose" to §21.9's
        // bare-discard reject, §22.5's default-value check and the §28.3
        // auto-drop gate alike. Absent in a pre-#3191 artifact → stays false →
        // exactly the pre-fix behaviour.
        t->importedNonTrivialDispose = td.nonTrivialDispose;
        t->invariantContracts = toContractRecords(td.invariants);
        t->genericParams = convertGenericParams(td.genericParams, t->memberScope);
        // fix #3151 (§2.8.3 rule 4, v0.252): the LOCAL side at
        // TypeResolution_Names.cpp L604 pushes to `baseTypes` AND
        // `baseTypeRefs` under ONE guard
        //   `kind == TypeRef::Kind::Named && ref->declaration`,
        // so the two vectors stay index-aligned for every local type. The
        // IMPORTER side here used to push to `baseTypeRefs` unconditionally
        // and to `baseTypes` only when the guard held — an asymmetry that
        // made baseTypeRefs longer than baseTypes for every imported type
        // whose metadata carried an UNRESOLVABLE named base (a transitive
        // dependency the importer does not load, or any name the importer's
        // `lookupNamed` returns null for). Every consumer that walks the
        // two vectors in lockstep (the indexed reads at
        // TypeResolution_Checks.cpp L2612 / L3123 / L4029 / L5335;
        // LlvmBackend_Disposal.cpp L3674; LlvmBackend_Layout.cpp L1748;
        // …) then pairs baseTypes[bi] with baseTypeRefs[bi] whose bi is
        // shifted by one — the resolved base's layout bound to the
        // unresolved base's TypeRef, the displacement measured on a plain
        // `D<long> : B<T>` case (see issue body).
        //
        // DESIGN (CLAUDE.md §4 — recorded decision, NOT a silent narrowing).
        // The issue offers two options:
        //   (1) importer pushes to BOTH vectors under one guard — same as
        //       local, and the choice taken here. Documented invariant
        //       ("parallel vectors") preserved. The unresolvable named base
        //       is dropped from BOTH lists, but the load-bearing
        //       "unresolvable-named" facts already have their own dedicated
        //       channels (`builtinExceptionBase` for #2950's exception base,
        //       `builtinInterfaceBases` for #3139's built-in interface
        //       bases) — both populated from their own MetaType fields, NOT
        //       from the baseTypes array, so this narrowing does not lose
        //       any of those facts.
        //   (2) push to BOTH unconditionally and tolerate null holes in
        //       baseTypes. Touches every `baseTypes` walk to add a null
        //       check; chose against because the documentation invariant
        //       ("parallel vectors, by index") already implies null-free
        //       and the holes would be load-bearing null vs. "missing"
        //       ambiguity.
        //
        // The TypeRef itself stays owned by `ctx` (it was allocated via
        // `ctx.makeImportedTypeRef()`), so the skip is a pure no-push, not
        // a leak — the reference lives for the context's lifetime, and the
        // importer does not surface it.
        for (const MetaType& base : td.baseTypes) {
            sema::TypeRef* ref = convert(base);
            if (ref->kind == sema::TypeRef::Kind::Named && ref->declaration) {
                t->baseTypeRefs.push_back(ref);
                t->baseTypes.push_back(ref->declaration);
            }
        }
        // fix #3139 (§9.8 / §18.5 / §21.4 / §2.8.3 rule 4, family F1 of
        // #3152): restore the BUILT-IN INTERFACE bases. Without them this
        // reconstructed symbol answers "declares none" to every conformance
        // consumer — and because those consumers had no symbol-side channel at
        // all and read `ast::TypeDecl::baseTypes`, which an imported type does
        // not have, a program the exporter compiles is REJECTED in the importer
        // ("no member `Serialize` on type `Pt`", the §18.5 format-spec reject,
        // the §9.8 component-eligibility reject).
        for (const MetaType& iface : td.builtinInterfaceBases)
            t->nonSymbolBaseRefs.push_back(convert(iface));
        for (const MetaMember& mm : td.members) {
            if (sema::Symbol* m = convertMember(mm, *t))
                t->members.push_back(m);
        }
        // fix #3144 (§35.1, family F9 of #3152): the choice's cases, appended
        // after the other members exactly as SymbolCollection::collectTypeDecl
        // appends them locally.
        convertChoiceCases(td, *t);
        return t;
    }

    // Reconstruct one member onto `parent`. Returns the symbol (already added by
    // caller) or null for member kinds not reconstructed in this increment.
    sema::Symbol* convertMember(const MetaMember& mm, sema::TypeSymbol& parent) {
        const sema::ScopeId scope = ctx.newScope();
        auto applyBehavior = [&](sema::MemberSymbol* member) {
            if (!member) return;
            member->isPure = mm.isPure;
            member->contractRecords = toContractRecords(mm.contracts);
            // fix #3140 (§9.8, family F2 of umbrella #3152): the §9.8
            // `serialized` clause. It had NO channel at all, so every §9.8
            // emitter recovered it by casting `Symbol::decl` — null for every
            // imported member — and answered "no clause" for all of them, which
            // put a `serialized skip` component back on the wire while the
            // header count still excluded it (the #1612 desync) and wrote a
            // renamed component under its declared name. `applyBehavior` runs
            // for every reconstructed member kind, so field, auto-property and
            // any future storage kind pick it up in one place.
            member->serializedSkip     = mm.serializedSkip;
            member->serializedOptional = mm.serializedOptional;
            if (!mm.serializedWireName.empty())
                member->serializedWireName =
                    ctx.internString(mm.serializedWireName);
            // §9.2 "The `equality skip` clause" (v0.307; follow-up to fix
            // #4643): the same one-place carriage, for the same reason. It had
            // NO channel at all, so every consumer recovered it by casting
            // `Symbol::decl` — null for every imported member — and answered
            // "participates in equality". MEASURED: an imported
            // `record Pt(X: int, Cache: int equality skip)` compared and hashed
            // `Cache` at the import while its own module excluded it, and the
            // body-record shape turned a locally-accepted `=` into a compile
            // error at the import. `applyBehavior` runs for every reconstructed
            // member kind, so field, positional field and auto-property all
            // pick it up here.
            member->equalitySkip = mm.equalitySkip;
            if (mm.isPure && std::find(member->modifiers.begin(),
                                      member->modifiers.end(), "pure") ==
                                  member->modifiers.end())
                member->modifiers.push_back(ctx.internString("pure"));
        };
        switch (mm.kind) {
            case MetaMemberKind::Method: {
                auto* m = ctx.makeImportedSymbol<sema::MethodSymbol>();
                m->name = ctx.internString(mm.name);
                m->visibility = fromMetaVisibility(mm.visibility);
                m->modifiers = intern(mm.modifiers);
                m->parent = &parent;
                m->scope = scope;
                m->isOperator = mm.isOperator;
                if (mm.isOperator) m->operatorToken = ctx.internString(mm.name);
                m->genericParams = convertGenericParams(mm.genericParams, scope);
                m->params = convertParams(mm.params, scope);
                // The method's resolved return type rides on a side table keyed
                // by ast::Type — but an imported method has no ast::Type node.
                // We stash it on the symbol via a dedicated map so overload
                // resolution / codegen can read it (see returnTypeOf below).
                returnTypes_[m] = convert(mm.type);
                returnOwned_[m] = mm.returnOwned;
                // fix #2391 (§14.2/§2.8.2, v0.236): capture the per-generic-
                // parameter constraint payload that the metadata builder
                // (ModuleMetadataBuilder::convertGenericParams, #1579/v0.182)
                // already exports on each MetaGenericParam, but that
                // `convertGenericParams` above (the import-side reconstructor)
                // silently drops — GenericParamSymbol carries only name/variance.
                // Without this side table, an override of an imported generic
                // base method skips the §14.2 constraint-restatement check
                // (checkOverrideSignatureMatch had no `bDecl` and no fallback),
                // so a mismatched-constraint override compiled clean. Aligned
                // positionally with `m->genericParams`.
                if (!mm.genericParams.empty()) {
                    std::vector<sema::ImportedGenericConstraint> gcs;
                    gcs.reserve(mm.genericParams.size());
                    for (const MetaGenericParam& mg : mm.genericParams) {
                        sema::ImportedGenericConstraint c;
                        c.typeParam = ctx.internString(mg.name);
                        for (const std::string& sc : mg.specialConstraints)
                            c.specialConstraints.push_back(
                                ctx.internString(sc));
                        for (const MetaType& ct : mg.constraintTypes)
                            c.constraintTypes.push_back(convert(ct));
                        gcs.push_back(std::move(c));
                    }
                    methodGenericConstraints_[m] = std::move(gcs);
                }
                applyBehavior(m);
                return m;
            }
            case MetaMemberKind::Field: {
                auto* f = ctx.makeImportedSymbol<sema::FieldSymbol>();
                f->name = ctx.internString(mm.name);
                f->visibility = fromMetaVisibility(mm.visibility);
                f->modifiers = intern(mm.modifiers);
                f->parent = &parent;
                f->scope = parent.memberScope;
                fieldTypes_[f] = convert(mm.type);
                // §2.5 / §22.8 (issue #328 FINDING [7]): a class-level field's
                // initializer runs as part of construction (step 2). In the
                // single-module path the initializer is emitted at the `new`
                // site; the importer has no AST, so a CONSTANT-FOLDABLE
                // initializer is carried textually in the metadata (like a
                // §22.12 const value) and replayed by the importer's lowerNew. A
                // non-foldable initializer (`new Array()`, `DateTime.Now`) is not
                // carried (valueText empty) — see ModuleMetadataBuilder.
                if (!mm.valueText.empty()) constValues_[f] = mm.valueText;
                // fix #2516 (§9.2/§9.3/§2.8.3 rule 4): a positional-record
                // field is reconstructed with a synthetic ast::Param whose
                // `type` is null, so `fs->posParam != nullptr` distinguishes
                // it from a body field at the sema layer (mirrors the LOCAL
                // path at SymbolCollection.cpp:295-313, which points posParam
                // at a real td.positionalFields[i]). `storageComponentType`'s
                // existing `if (fs->posParam && fs->posParam->type)` gate
                // returns false for the synthetic case (posParam->type is
                // null) and falls through to `ctx.importedMemberType(m)` —
                // the importer's `fieldTypes_` side table — so no downstream
                // consumer dereferences the synthetic Param. The Param is
                // owned by the SemaContext's `importedSymbols_` arena so it
                // lives at least as long as the FieldSymbol.
                if (mm.isPositional) {
                    auto* p = ctx.makeImportedParam();
                    f->posParam = p;
                    // fix #3750 (§22.5 / §22.10 / §28.3 rule 5 / §28.9,
                    // v0.286): the synthetic `posParam` reconstructed here is
                    // the mirror of the LOCAL path's `posParam = &pf`
                    // (SymbolCollection.cpp:309), whose modifier `""` /
                    // `"owned"` rides on `ast::Param::modifier`. Without
                    // bridging `mm.modifiers[0]` ("owned", the only marker
                    // the metadata carries — ModuleMetadataBuilder
                    // line 1167-1168) into `p->modifier`, an imported §22.10
                    // base call's #3750 reject predication saw `modifier.empty()`
                    // for an `owned r: Res` field — false-rejected the
                    // borrowed-over-imported-base sibling shape (LSan-free
                    // rows that the metadata has always supported). The
                    // `modifiers`-list path also carries `static` for body
                    // fields; a positional class parameter carries `owned`
                    // ONLY in current spec, so reading the first element is
                    // safe. Recorded here because every other reader of
                    // `fs->posParam->modifier` (#2654 / #3773 / the
                    // `Modifier` arm of `formalParamName` /
                    // `formalParamHasDefault`) currently answers
                    // "plain"/"no default" for every imported positional
                    // parameter — a silent-write the predicate carried for
                    // one site (#3647) before this fix.
                    if (!mm.modifiers.empty()) {
                        p->modifier = ctx.internString(mm.modifiers.front());
                    }
                    // fix #3221 (§22.10/§9.8/§2.8.3 rule 4, v0.254): the
                    // captured-vs-not flag. The synthetic `posParam` above
                    // (#2516) is the positional vs body distinction; the
                    // §22.10 capture enumeration (a body-referenced
                    // parameter becomes a private instance field) is a
                    // SEPARATE flag, `posParamBodyRef`, that #2325 computes
                    // locally and nothing in the loader previously bridged.
                    // Without it the importer's
                    // `isInstanceFieldStorageSymbol` (fix #3182) answers
                    // FALSE on every imported captured parameter, and an
                    // imported base whose only storage is a captured primary-
                    // ctor parameter slips past #2364's base-with-storage
                    // guard. The bit rides as the 18th trailing scalar in
                    // MEMBER_BLOCK (the same additive shape isPositional
                    // rides in), and a pre-#3221 blob reads as `false` —
                    // matching the dependency's own pre-fix behavior. A
                    // record's positional field cannot reach this: their
                    // parameters have no body references and
                    // `posParamBodyRef` is structurally false on both sides.
                    if (mm.posParamBodyRef)
                        f->posParamBodyRef = true;
                    // fix #3202 (§22.10 "primary constructors" / §22.5
                    // "Optional parameters and named arguments" / §2.8.3
                    // rule 4, v0.254): the §22.10 primary-constructor
                    // parameter's `:= Expr` default, textualised. Without
                    // this, the importer cannot tell a primary parameter is
                    // OPTIONAL at all — `checkPositionalNewArgsFromSymbols`
                    // rejects `new DefBox(1)` against
                    // `class DefBox(A: int, B: int := 9)` with "required
                    // parameter `B` is not supplied", which is false of `B`.
                    // The text rides on the SYNTHETIC `posParam` (#2516) so
                    // codegen's runtime default arm can fold it through the
                    // same `importedDefaultArgValue` machinery method
                    // parameters already use. Empty → no default, which is
                    // exactly the pre-fix behaviour a stale dependency
                    // already exhibits — its primary parameters stay
                    // required, and the same reject that surfaces today
                    // stays. A record's positional field cannot reach this
                    // either: `parsePositionalFieldList` admits no default
                    // syntax (A.3 `PositionalField ::= Identifier ":" Type
                    // SerializedClause? EqualityClause?`), so
                    // `posParamDefaultText` is structurally empty for every
                    // record (the §9.1 no-defaults rule is enforced at the
                    // grammar on the local side; the encoder's `if
                    // (fs->posParam->defaultValue)` is also structurally
                    // false for record positional fields).
                    p->defaultText = mm.posParamDefaultText;
                }
                applyBehavior(f);
                return f;
            }
            case MetaMemberKind::Const: {
                auto* c = ctx.makeImportedSymbol<sema::ConstMemberSymbol>();
                c->name = ctx.internString(mm.name);
                c->visibility = fromMetaVisibility(mm.visibility);
                c->modifiers = intern(mm.modifiers);
                c->isStatic = true;  // §22.12: a class-level const is static
                c->parent = &parent;
                c->scope = parent.memberScope;
                fieldTypes_[c] = convert(mm.type);
                // §22.12 (issue #328 FINDING [3] BUG A): carry the const's
                // compile-time value so codegen folds it at the read site (the
                // library emits no storage symbol for a const). Recorded only
                // when the builder rendered a value (non-empty).
                if (!mm.valueText.empty()) constValues_[c] = mm.valueText;
                // fix #3230 (§17.14 "Constant expressions" / §15.1 "Inline
                // arrays" / §22.12 "Static members"; family M11 of umbrella
                // #3152): an IMPORTED integer-typed const has no AST TypeDecl
                // (the symbol is reconstructed from dependency metadata),
                // so the sema fold closures that walk `cms->decl->value`
                // silently return nullopt for it and `arraySizeValue` /
                // case-pattern ranges / §5.1 narrowing silently miss the
                // fold. Parse the textual value (the SAME wire text the
                // existing `constValues_` channel already carries — fix
                // #2898's "FOLD, don't render") into a typed int64 and
                // attach it to the symbol. Only set when the const's
                // declared type is an integer-family primitive (§4 bytes
                // through ulong); string/bool/char/float/decimal consts
                // leave it nullopt and continue to fold through the
                // existing codegen `foldImportedConstText` path unchanged.
                if (!mm.valueText.empty() && mm.type.kind == TypeKind::Primitive) {
                    const sema::PrimitiveType p =
                        fromPrimitiveKind(mm.type.primitive);
                    using P = sema::PrimitiveType;
                    if (p == P::Byte || p == P::SByte || p == P::Short ||
                        p == P::UShort || p == P::Int || p == P::UInt ||
                        p == P::Long || p == P::ULong) {
                        c->intValue = parseImportedConstIntText(mm.valueText);
                    }
                }
                applyBehavior(c);
                return c;
            }
            case MetaMemberKind::EnumConstant: {
                auto* e = ctx.makeImportedSymbol<sema::EnumConstantSymbol>();
                e->name = ctx.internString(mm.name);
                e->visibility = fromMetaVisibility(mm.visibility);
                e->parent = &parent;
                e->scope = parent.memberScope;
                // §10/§10.2 (issue #328 FINDING [1]): carry the constant's
                // underlying integer value from the dependency metadata so
                // codegen lowers `int(Color.Green)` / enum equality / `case`
                // arms to the real value instead of 0. The builder always
                // records hasEnumValue for a literal/continuation constant;
                // when it could not (a non-literal const-expr initializer) the
                // value stays 0, matching the metadata's default.
                if (mm.hasEnumValue) e->value = mm.enumValue;
                // fix #3140 (§9.8, family F4 of umbrella #3152): the §9.8 wire
                // NAME. The VALUE above has crossed since #328, but the wire
                // name had no channel at all, so `emitEnumSerialize` read it
                // off the `ast::EnumConstant` — which an imported enum does not
                // have — and an imported enum component serialized to NOTHING.
                // Empty when the constant carries no `serialized` clause (and
                // for a pre-#3140 dependency blob), which is exactly §9.8's
                // "the wire name is the declared name".
                if (!mm.enumWireName.empty())
                    e->serializedName = ctx.internString(mm.enumWireName);
                applyBehavior(e);
                return e;
            }
            case MetaMemberKind::Constructor: {
                // §2.5 / §22.7 (issue #328 FINDING [7]): a public constructor is
                // part of the module's public interface. Reconstruct it as a
                // ConstructorSymbol so `new ImportedType(args)` resolves against
                // the real signature (arity + arg types — §22.6), instead of the
                // importer assuming only the implicit 0-arg default. The ctor's
                // parameters become ParameterSymbols whose resolved types ride
                // `resolvedDeclType`, matching the imported-method path. A static
                // constructor (§22.7) is not part of the callable surface — it
                // runs once in the EXPORTER's static-init; it is filtered out by
                // the builder (only instance ctors are exported), but guard here
                // too via the "static" modifier for fidelity.
                auto* c = ctx.makeImportedSymbol<sema::ConstructorSymbol>();
                c->name = ctx.internString(mm.name);
                c->visibility = fromMetaVisibility(mm.visibility);
                c->modifiers = intern(mm.modifiers);
                c->parent = &parent;
                c->scope = scope;
                for (const std::string& md : mm.modifiers)
                    if (md == "static") c->isStatic = true;
                c->params = convertParams(mm.params, scope);
                applyBehavior(c);
                return c;
            }
            case MetaMemberKind::Property: {
                // §2.5 / §22.9 (issue #503): reconstruct a PropertySymbol so
                // that cross-module `obj.Prop` resolves to a real symbol instead
                // of falling through member lookup and returning 0. The getter
                // BODY is not reconstructed here (there is no ast::PropertyDecl
                // in the importer); codegen handles the getter call separately:
                //   - virtual/override: vtable-slot dispatch (propGetSlot_),
                //     populated by buildVtable via isVirtualProperty() which
                //     reads the modifiers reconstructed here.
                //   - non-virtual: a call to bz$<Class>$<Prop>$propget, emitted
                //     as ExternalLinkage by the exporter and declared as an
                //     external by declareImportedPropertyGetters in the importer.
                // The resolved property TYPE rides fieldTypes_ (same side table
                // used for fields/consts) so ctx.importedMemberType() can read it.
                //   - the SETTER (fix #2400): non-virtual — a call to
                //     bz$<Class>$<Prop>$propset, emitted as ExternalLinkage by
                //     emitExportedPropertySetters and declared as an external
                //     by declareImportedPropertySetters. Whether such a call
                //     is even attempted depends on mm.hasSetter (below) — an
                //     imported property with no setter must never resolve one,
                //     the same way propertyHasSetter's LOCAL check does.
                auto* p = ctx.makeImportedSymbol<sema::PropertySymbol>();
                p->name = ctx.internString(mm.name);
                p->visibility = fromMetaVisibility(mm.visibility);
                p->modifiers = intern(mm.modifiers);
                p->parent = &parent;
                p->scope = parent.memberScope;
                p->isIndexer = false;
                fieldTypes_[p] = convert(mm.type);
                // fix #2400 (§23/§2.8.2/§2.8.3 rule 4): MetaMember::hasSetter,
                // published to ctx.importedPropertyHasSetter() below. See the
                // field's comment in ModuleMetadata.hpp for why this cannot be
                // derived from `p->modifiers` alone.
                propHasSetter_[p] = mm.hasSetter;
                // fix #2461 (§22.3/§23): MetaMember::hasGetter, published to
                // ctx.importedPropertyHasGetter() below — the getter-presence
                // twin of hasSetter, needed so checkRefOutPropertyOperand can
                // reject a write-only imported property used as a `ref`
                // argument (§22.3: ref's copy-in calls the getter) with a
                // clean diagnostic instead of falling through to a raw linker
                // error (undefined `...$propget`).
                propHasGetter_[p] = mm.hasGetter;
                // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4):
                // MetaMember::setterVisibility, published to
                // ctx.importedPropertySetterVisibility() below — the
                // setter's EFFECTIVE visibility (`private set` etc.), needed
                // because an imported PropertySymbol has no ast::PropertyDecl
                // to read `pd->accessors[i].visibility` from at the
                // cross-module assignment site.
                propSetterVisibility_[p] = fromMetaVisibility(mm.setterVisibility);
                // fix #2478 (§9.2/§22.8): MetaMember::isAutoProperty,
                // published to ctx.importedPropertyIsAutoStorage() below —
                // whether this Property is a real instance-storage member
                // (backed by compiler-generated storage, §9.2) rather than a
                // computed (expression/block-bodied) property. Needed
                // because `collectInstanceStorage`/`collectRecordStorage`
                // (LlvmBackend_impl.hpp) must know this to lay out an
                // imported type's storage correctly — the decl-dependent
                // `sema::hasAstAutoPropertyDecl` always returns false here
                // (no ast::PropertyDecl crosses the boundary).
                propIsAutoStorage_[p] = mm.isAutoProperty;
                // fix #3618 (§22.15 / §2.8.2): the accessor-level `pure get`
                // marker — the spelling §22.15 FORCES on every read-write
                // block-form property (TypeResolution_Checks.cpp:11524
                // REJECTS property-level `pure` on a Block bodyKind with a
                // setter, citing "use per-accessor `pure get` instead"). The
                // property-level `pure` (read-only/expression-bodied shape)
                // already crosses the wire via `member->isPure` (set above by
                // `applyBehavior` reading `mm.isPure`); this slot covers the
                // accessor spelling that NEVER appears in the property's
                // modifier list. Without it the importer-side
                // `propertyGetterIsPure` returned `false` on every imported
                // `pure get` while the exporter's own compilation of the
                // character-identical source returned `true` — silent
                // cross-module wrong answer to §22.15 rule 6. Consulted via
                // `propertyGetterIsPure` ahead of the AST walk (the AST is
                // null for an imported PropertySymbol, so the walk would
                // have answered "no").
                p->accessorIsPure = mm.accessorIsPure;
                // fix #3630 narrow (§22.15 v0.286, over-reject follow-up):
                // carry the existing `MetaMember::isAutoProperty` wire bit
                // (#2478's 13th scalar) onto the importer-side
                // `PropertySymbol::isAutoProperty` so the imported arm of
                // `propertyGetterIsPure` admits an auto-property's getter
                // (the synthesized `return backing-field` shape is pure-
                // by-construction). Without this, #3630's MemberAccessExpr
                // arm over-rejects every read of an imported auto-property
                // inside a pure body — the regression measured on
                // xmod_pure_ctor_chain_3174.t5 and xmod_record_sibling_ctor_3219.
                p->isAutoProperty = mm.isAutoProperty;
                // fix #2467 (§9.2/§2.5, v0.235): mirror the Field case's
                // constValues_ carriage above — an auto-property's
                // constant-foldable initializer (ModuleMetadataBuilder.cpp's
                // new Property-branch `out.valueText`) is folded at RECORD
                // construction by buildRecordValue's decl-null fallback
                // (LlvmBackend_Exprs2.cpp), the same `ctx_.importedConstValue`
                // / `foldImportedConstText` path §22.12/§22.8 FINDING [3]/[7]
                // already provide. No consumer exists on the CLASS side (a
                // class replays field/auto-property initializers via the
                // exported `bz$<Type>$init` function, #2452) — this entry
                // simply rides unread for a class's PropertySymbol, exactly
                // as the pre-existing Field-side carriage already does.
                if (!mm.valueText.empty()) constValues_[p] = mm.valueText;
                applyBehavior(p);
                return p;
            }
            case MetaMemberKind::NestedType: {
                // §11.3 (schema v4, issue #502): reconstruct a NestedTypeSymbol
                // with a fully populated inner TypeSymbol so that `new Outer.Inner()`
                // and `Outer.Inner.Method()` resolve in the importer.
                // The inner TypeSymbol's members (including their return types in
                // returnTypes_ / fieldTypes_) are populated by the recursive
                // convertMember calls inside reconstructTypeSymbol.
                if (!mm.nestedType) return nullptr;  // malformed (no inner decl)
                auto* nts = ctx.makeImportedSymbol<sema::NestedTypeSymbol>();
                nts->name = ctx.internString(mm.name);
                nts->visibility = fromMetaVisibility(mm.visibility);
                nts->modifiers = intern(mm.modifiers);
                nts->parent = &parent;
                nts->scope = parent.memberScope;
                nts->type = reconstructTypeSymbol(*mm.nestedType, parent);
                applyBehavior(nts);
                return nts;
            }
            case MetaMemberKind::Indexer: {
                // §22.10 / §23.4 (issue #2414): reconstruct an indexer
                // PropertySymbol so that cross-module `obj[i]` resolves to a
                // real symbol instead of falling through member/type
                // resolution and reading garbage (confirmed: 252 instead of
                // 3). Mirrors the Property case above but additionally
                // reconstructs `indexerParams` from `mm.params` (the
                // exporter's `convertParams(prop->indexerParams, ctx)`
                // counterpart, ModuleMetadataBuilder.cpp) and sets
                // `isIndexer = true` so `PropertySymbol::isIndexer`-gated
                // logic (sema's overload selection, codegen's
                // findIndexer/selectIndexer/lowerIndexerGet/-Set) recognizes
                // it as an indexer rather than a named property. As with
                // Property, there is no ast::PropertyDecl to reconstruct — a
                // decl-null fallback is added at each consuming site (sema
                // candidate matching/typing; codegen non-virtual class-direct
                // dispatch) rather than inventing a synthetic AST node.
                auto* p = ctx.makeImportedSymbol<sema::PropertySymbol>();
                p->name = ctx.internString(mm.name);
                p->visibility = fromMetaVisibility(mm.visibility);
                p->modifiers = intern(mm.modifiers);
                p->parent = &parent;
                p->scope = parent.memberScope;
                p->isIndexer = true;
                p->indexerParams = convertParams(mm.params, scope);
                fieldTypes_[p] = convert(mm.type);
                // fix #2400's bit, reused for indexers per its own field
                // comment in ModuleMetadata.hpp ("Ignored for every kind but
                // Property/Indexer").
                propHasSetter_[p] = mm.hasSetter;
                // fix #2461's bit, reused for indexers per its own field
                // comment in ModuleMetadata.hpp — a write-only (`set`-only)
                // indexer is equally illegal as a `ref` target under §22.3.
                propHasGetter_[p] = mm.hasGetter;
                // fix #2415's bit, reused for indexers per its own field
                // comment in ModuleMetadata.hpp ("Ignored for every kind but
                // Property/Indexer").
                propSetterVisibility_[p] = fromMetaVisibility(mm.setterVisibility);
                // fix #2478's bit: an indexer is never "auto" (the exporter's
                // own `out.isAutoProperty` computation excludes
                // `prop->isIndexer` unconditionally), so this is always
                // false here — set for consistency with hasSetter/
                // setterVisibility above, not because an indexer can occupy
                // a storage slot.
                propIsAutoStorage_[p] = mm.isAutoProperty;
                // fix #3618 (§22.15 / §2.8.2): accessor-level `pure get`
                // twin of the Property case above — an indexer's `pure get`
                // getter also rides `MetaMember::accessorIsPure` and
                // reaches the importer here (the symbol-side fallback the
                // Property case above puts on `p->accessorIsPure` is the
                // same field on the imported PropertySymbol — indexers are
                // PropertySymbols with `isIndexer = true`, mirror of the
                // hasSetter/hasGetter/etc. twin above).
                p->accessorIsPure = mm.accessorIsPure;
                // fix #3630 narrow: twin of the Property case above — an
                // indexer's auto-property bit rides `mm.isAutoProperty` too.
                // (The exporter excludes indexers, so this is always false
                // here; carried for predicate symmetry — `propertyGetterIsPure`
                // consults a single `ps->isAutoProperty` field regardless of
                // `ps->isIndexer`.)
                p->isAutoProperty = mm.isAutoProperty;
                applyBehavior(p);
                return p;
            }
            case MetaMemberKind::Event: {
                // §11.2 / §24 (issue #2407): reconstruct an EventSymbol for
                // an imported type's `event` member. §24 (v0.235) retires the
                // `event` member kind ("the word `event` remains reserved...
                // an `event` declaration is a reserved-word error") in favor
                // of the IEvent<T>/EventSource<T> stdlib §6.5 pattern — but
                // the parser has not yet caught up to that retirement (still
                // accepts `event` declarations, Parser_Decls.cpp), and the
                // compiler implements the legacy member kind end-to-end for
                // LOCAL members (parse/sema/codegen) and, since #2401, mirrors
                // imported-INTERFACE event itable indices. Reconstructing the
                // symbol here completes that already-committed legacy surface
                // for cross-module parity; it does not resolve #2407's own
                // flagged design question (complete the legacy surface vs.
                // implement the retirement) — that remains open, per CLAUDE.md
                // rule 4, for explicit human direction.
                //
                // SCOPE (see xmod_event_import_2407_test.cpp's file header for
                // the full account, and #2421 for the filed follow-up): this
                // symbol reconstruction alone does NOT make cross-module
                // Subscribe/Raise work end-to-end. TWO further decl-null
                // gates block it, both requiring a NEW metadata bit this fix
                // does not add (a wrong guess would risk an itable/struct-
                // layout mismatch between exporter and importer — the exact
                // #2406-style hazard #2420 already caught elsewhere in this
                // same session):
                //   1. collectInstanceStorage (LlvmBackend_impl.hpp) needs
                //      `dynamic_cast<ast::EventDecl>(es->decl)` to even ADD
                //      the event's backing-field slot to instance storage —
                //      unconditionally null for an imported EventSymbol, so
                //      the reconstructed event still gets NO fieldIndex_
                //      entry, exactly as before this fix (this fix does not
                //      change that outcome, only whether the SYMBOL exists).
                //   2. #2401's ifaceEventIndex_ mirror (LlvmBackend_Setup.cpp)
                //      needs the same `ed`/`!ed->isAsync` check to reserve an
                //      itable slot — also unconditionally null.
                // Both need an `isStatic`/`isAsync` metadata bit
                // (EventSymbol::modifiers is always empty even for a LOCAL
                // event — SymbolCollection.cpp's EventDecl case never copies
                // it — so neither is recoverable from `mm.modifiers` either).
                auto* e = ctx.makeImportedSymbol<sema::EventSymbol>();
                e->name = ctx.internString(mm.name);
                e->visibility = fromMetaVisibility(mm.visibility);
                e->modifiers = intern(mm.modifiers);
                e->parent = &parent;
                e->scope = parent.memberScope;
                fieldTypes_[e] = convert(mm.type);  // §24 payload type
                // fix #2421 (§24/§2.8.2): record the wire-carried static/
                // async classification bits so the codegen event-dispatch
                // path's eventIsStatic/eventIsAsync helpers classify this
                // imported event IDENTICALLY to the exporter (avoiding an
                // itable/struct-layout mismatch). Published to the SemaContext
                // side table below; see MetaMember::isEventStatic/isEventAsync.
                eventIsStatic_[e] = mm.isEventStatic;
                eventIsAsync_[e] = mm.isEventAsync;
                applyBehavior(e);
                return e;
            }
            default:
                return nullptr;
        }
    }

    // Side tables: an imported member's resolved type cannot ride the
    // ast::Type-keyed SemaContext side table (there is no ast::Type), so the
    // loader records them here and the public accessors below expose them.
    std::unordered_map<sema::MethodSymbol*, sema::TypeRef*> returnTypes_;
    std::unordered_map<sema::MethodSymbol*, bool> returnOwned_;
    std::unordered_map<const sema::MemberSymbol*, sema::TypeRef*> fieldTypes_;
    // fix #2391 (§14.2/§2.8.2, v0.236): imported generic base method's
    // per-parameter constraint payload, from MetaMember::genericParams (the
    // builder's `convertGenericParams` has carried `constraintTypes`/
    // `specialConstraints` per param since #1579/v0.182). Published to
    // ctx.setImportedGenericConstraints() below so
    // checkOverrideSignatureMatch's §14.2 override-restatement leg can verify
    // an override against an imported generic base. Positionally aligned with
    // the method's `MethodSymbol::genericParams`.
    std::unordered_map<const sema::MethodSymbol*,
                       std::vector<sema::ImportedGenericConstraint>>
        methodGenericConstraints_;
    // fix #2400 (§23/§2.8.2/§2.8.3 rule 4): imported property setter presence,
    // from MetaMember::hasSetter — published to
    // ctx.setImportedPropertyHasSetter() below.
    std::unordered_map<const sema::MemberSymbol*, bool> propHasSetter_;
    // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4): imported
    // property/indexer setter EFFECTIVE visibility, from
    // MetaMember::setterVisibility — published to
    // ctx.setImportedPropertySetterVisibility() below.
    std::unordered_map<const sema::MemberSymbol*, ast::Visibility>
        propSetterVisibility_;
    // fix #2478 (§9.2/§22.8): imported property AUTO-storage classification,
    // from MetaMember::isAutoProperty — published to
    // ctx.setImportedPropertyIsAutoStorage() below.
    std::unordered_map<const sema::MemberSymbol*, bool> propIsAutoStorage_;
    // fix #2461 (§22.3/§23): imported property/indexer getter presence, from
    // MetaMember::hasGetter — published to ctx.setImportedPropertyHasGetter()
    // below.
    std::unordered_map<const sema::MemberSymbol*, bool> propHasGetter_;
    // fix #2421 (§24/§2.8.2): imported event static/async classification,
    // from MetaMember::isEventStatic/isEventAsync — published to
    // ctx.setImportedEventIsStatic/Async() below.
    std::unordered_map<const sema::MemberSymbol*, bool> eventIsStatic_;
    std::unordered_map<const sema::MemberSymbol*, bool> eventIsAsync_;
    // §22.12 imported const values (issue #328 FINDING [3] BUG A), textual.
    std::unordered_map<const sema::MemberSymbol*, std::string> constValues_;
};

// --- §14/#662c: imported generic ctor/dtor body re-parsing ------------------
//
// For an imported generic CLASS whose ConstructorDecl source text was exported
// (schema v5, `ctorBodyText`), re-parse the body in a fresh SemaContext, then
// remap the fresh symbols to the already-reconstructed imported symbols so that
// `ctx_.exprSymbol(e)` in the MAIN SemaContext returns the imported FieldSymbol
// or ParameterSymbol — letting inlineCtorBody / implicitFieldAddr work with the
// per-instantiation genericSubst_ active (§14.1 monomorphization).

// fix #3540 (§11.6 / §2.8.3 rule 3, v0.285): the last component of a Named
// MetaType's `namedName`, i.e. the type's SIMPLE identifier.
//
// `namedName` carries the QUALIFIED type name ("Outer.Box" for a §11.6 nested
// type, identical to the simple name for a top-level one) because that is what
// §2.8.3 rule 3 identifies a type by and what the loader's by-name resolution
// needs to tell `Outer1.Box` from `Outer2.Box`. Three consumers want the simple
// spelling instead, and they ask through this one helper so they cannot drift
// from each other:
//
//   * `metaTypeToSourceText` renders SYNTHETIC stub source whose class header
//     declares the SIMPLE name (`class Box<T>`) and which has no `Outer` in
//     scope, so a qualified spelling would not resolve inside it;
//   * `metaTypeSigText` is a signature key compared against `typeRefSigText`,
//     which renders `declaration->name` — the simple name — so the two must
//     produce the same text for the same type or ctor/dtor body attachment
//     (§14/#705) silently stops matching;
//   * `metaTypeReferencesNonSelfNamed` compares against that same synthetic
//     header's `selfName`, which is `MetaTypeDecl::name`, again simple.
//
// Reading through it makes all three byte-identical to their pre-#3540
// behaviour for every program, nested or not; only type RESOLUTION sees the
// qualified form.
static std::string metaNamedSimpleName(const MetaType& mt) {
    const std::size_t dot = mt.namedName.rfind('.');
    return dot == std::string::npos ? mt.namedName
                                    : mt.namedName.substr(dot + 1);
}

// Render a MetaType as Bezel source text for use in a synthetic class source.
static std::string metaTypeToSourceText(const MetaType& mt) {
    if (mt.kind == TypeKind::Primitive) {
        std::string s;
        switch (mt.primitive) {
            case PrimitiveKind::Bool:    s = "bool";    break;
            case PrimitiveKind::Char:    s = "char";    break;
            case PrimitiveKind::String:  s = "string";  break;
            case PrimitiveKind::Byte:    s = "byte";    break;
            case PrimitiveKind::SByte:   s = "sbyte";   break;
            case PrimitiveKind::Short:   s = "short";   break;
            case PrimitiveKind::UShort:  s = "ushort";  break;
            case PrimitiveKind::Int:     s = "int";     break;
            case PrimitiveKind::UInt:    s = "uint";    break;
            case PrimitiveKind::Long:    s = "long";    break;
            case PrimitiveKind::ULong:   s = "ulong";   break;
            case PrimitiveKind::Float:   s = "float";   break;
            case PrimitiveKind::Double:  s = "double";  break;
            case PrimitiveKind::Decimal: s = "decimal"; break;
            case PrimitiveKind::Object:  s = "Object";  break;
            default:                     s = "int";     break;
        }
        // fix #4516 (§7.1 / §7.2 / §22.6, v0.306): render the nullable `?` on a
        // PRIMITIVE, as the Named arm below has always done. Dropping it here
        // replayed `x: int?` as `x: int` — a declared type the exporter never
        // wrote — and, because this text is the source the §14/#705 AST-side
        // key is re-parsed from, it also made nullability UNRENDERABLE in the
        // key: `Pick(a: int)` beside `Pick(b: int?)` emitted two IDENTICAL
        // stubs, which is a duplicate-member error that takes the whole
        // reconstruction down (MEASURED: §14/#662 rejection of a program the
        // exporter compiled). `string` and `string?` are distinct types (§7.1)
        // and `int?` is a distinct type from `int` (§7.2), so §22.6's
        // "overloads differing in ... types are distinguishable" covers the
        // pair and the stub must spell the two apart.
        if (mt.nullable) s += "?";
        return s;
    }
    if (mt.kind == TypeKind::Named) {
        std::string s = metaNamedSimpleName(mt);  // fix #3540 — see the helper
        if (!mt.genericArgs.empty()) {
            s += "<";
            for (std::size_t i = 0; i < mt.genericArgs.size(); ++i) {
                if (i) s += ", ";
                s += metaTypeToSourceText(mt.genericArgs[i]);
            }
            s += ">";
        }
        if (mt.nullable) s += "?";
        return s;
    }
    // fix #4480 (§3.1, §15.1, §27.1, Appendix A.5, v0.306): render the three
    // remaining renderable shapes instead of substituting `Object`. This
    // function produces the SYNTHETIC RECONSTRUCTION SOURCE an importer
    // re-parses (field-initializer replay #2874/#3671/#3723, ctor/dtor body
    // re-parse #662c, base-clause argument re-parse #3911), so a placeholder
    // here silently replays an exporter's declaration at a type the exporter
    // never wrote. For a function-typed field that happened to be harmless —
    // a function type is a reference type deriving from `Object` (§3), the
    // replayed lambda re-announces its own parameter types, and the slot is a
    // `ptr` either way — but none of those three mitigations hold for an
    // inline array, whose type is neither a reference type nor `ptr`-shaped.

    // §3.1 / A.5 `TypeNoNullable ::= … | "(" ")"`: the unit type. Reached
    // mainly as a function type's return (`(string) -> ()`); a method's own
    // unit return is already omitted by buildMethodStub before it gets here.
    // fix #4516: `Type ::= TypeNoNullable "?"?` (A.5) admits `()?` and the
    // parser accepts it, so the nullable form is rendered here for the same
    // reason it is on every other spellable kind.
    if (mt.kind == TypeKind::Unit) return mt.nullable ? "()?" : "()";

    // §15.1 / A.5 `InlineArrayType ::= "[" ConstIntExpr "owned"? "]" Type`.
    // The size is textual on the wire and already a folded decimal (#3183).
    // An inline array has NO nullable form (A.5), so `nullable` is not
    // rendered. An UNFOLDABLE size reaches the wire empty — #3183 keeps that
    // shape failing loudly rather than acquiring a wrong number, and there is
    // no source spelling for an unknown extent — so it falls through below.
    if (mt.kind == TypeKind::InlineArray && !mt.arraySize.empty() &&
        mt.children.size() == 1) {
        std::string s = "[" + mt.arraySize;
        if (mt.arrayOwned) s += " owned";
        s += "]";
        s += metaTypeToSourceText(mt.children[0]);
        return s;
    }

    // §27.1 / A.5 `FunctionType ::= "(" (FuncParamType …)? ")" "->" "owned"?
    // Type`, with `children[0..n-1]` the parameters and `children[n]` the
    // return (ModuleMetadata.hpp).
    //
    // Parameter MODIFIERS are rendered and parameter NAMES are not: §27.1
    // makes the modifier "part of the type" — "`(T) -> ()` and `(owned T) ->
    // ()` are distinct function types" — while its own table spells every
    // function type without names (`method Foo(x: int): bool` ↔ `(int) ->
    // bool`), so a name is decoration that A.5 admits and identity ignores.
    //
    // §27.2: the nullable form must be PARENTHESISED — `((int) -> bool)?` —
    // because in `-> T?` the `?` binds to the return type.
    if (mt.kind == TypeKind::Function && !mt.children.empty()) {
        const std::size_t n = mt.children.size() - 1;  // children[n] = return
        std::string s = "(";
        for (std::size_t i = 0; i < n; ++i) {
            if (i) s += ", ";
            if (i < mt.funcParamMods.size() && !mt.funcParamMods[i].empty())
                s += mt.funcParamMods[i] + " ";
            s += metaTypeToSourceText(mt.children[i]);
        }
        s += ") -> ";
        if (mt.funcReturnOwned) s += "owned ";
        s += metaTypeToSourceText(mt.children[n]);
        if (mt.nullable) s = "(" + s + ")?";
        return s;
    }

    // What remains has no source spelling at all: the `Error` sentinel (a type
    // the exporter could not resolve — "should not normally be emitted",
    // ModuleMetadata.hpp), an inline array whose extent did not fold, and a
    // structurally malformed Function/InlineArray. The placeholder is kept for
    // those rather than emitting text that cannot parse: a stub that fails to
    // parse would take down reconstruction for the whole type, where a wrong
    // declared type on one member surfaces at that member's own use site.
    return "Object";
}

// §14/#705: canonical type text for a MetaType, used as a signature key for
// overload-safe matching (not for source-code generation — that is
// metaTypeToSourceText).  Uses ',' without space for generic args, and renders
// the nullable '?' suffix (fix #4516).
//
// NULLABILITY IS PART OF THE KEY (fix #4516, §7.1 / §7.2 / §22.6, v0.306).
// This comment used to read "omits the nullable '?' suffix: stubs do not
// reliably preserve nullability for primitive types, and nullable-vs-non-
// nullable overloads are rare". Both halves were defects rather than reasons.
// §7.1: "`string` and `string?` are distinct types"; §7.2 says the same of a
// nullable value type, and §14.2 leans on it ("a nullable value type is a
// distinct type from its non-nullable form (§7.1)"). §22.6 opens "Overload
// resolution considers number, TYPES, the `ref`/`out` modifiers, and the
// presence of `owned` on an ordinary parameter — overloads differing in any of
// these are distinguishable", and closes with the same list — so a pair
// differing only in nullability is legal, and the compiler accepts it
// (MEASURED: a single module resolves `Pick(a: T)` beside `Pick(b: T?)` to the
// non-nullable member and answers 105). The "stubs do not preserve it" half
// was true of `metaTypeToSourceText`'s PRIMITIVE arm only, and that arm is the
// other half of this fix: it now renders the `?` the Named and Function arms
// always did, so all three sides can see it.
//
// MEASURED pre-fix, real `bezelc`, a `-c` library plus an importing executable:
//   * `Pick(a: SC<T>)` beside `Pick(b: SC<T>?)` — single module 105, imported
//     205, the OTHER overload's body. The collision of facet (a): a silent
//     wrong value across a module boundary.
//   * `Pick(a: int)` beside `Pick(b: int?)` — single module 105, imported
//     REJECTED with §14/#662, because both stubs rendered `int` and the
//     synthetic source declared the same method twice. Facet (b).
//
// An InlineArray is excluded: A.5 gives it no nullable form (`([2]int)?` does
// not parse — MEASURED), so `nullable` cannot be true for one and rendering it
// would be a spelling no side could produce.
//
// THE PARTICIPATING SET IS FOUR FUNCTIONS, not the three below. For obligation
// (b) `metaTypeToSourceText` is a participant too: the AST side re-parses ITS
// output, so a facet it cannot spell is a facet the AST key can never carry,
// whatever these three do. That is why #4508 could not render parameter names
// and why nullability could not be rendered before this fix — and why all four
// move in one commit.
//
// THE KEY HAS TWO OBLIGATIONS, and this function is one of THREE renderers that
// must discharge both together (fix #4508):
//
//   (a) two DIFFERENT signatures must get DIFFERENT keys — otherwise the
//       last-writer / first-match of the attachment loops binds an exporter's
//       body to the wrong imported overload;
//   (b) the SAME signature must get the SAME key on BOTH SIDES of whichever
//       comparison it is in — otherwise attachment silently stops and the
//       importer rejects a program the exporter compiled (§14/#662).
//
// The comparisons, and so the renderers that must stay byte-identical:
//   * metaMemberSigKey (this function)  vs  freshMethodSigKey (astTypeToSigText)
//     — MetaType vs the RE-PARSED SYNTHETIC STUB's AST, at the importedMethods
//     map;
//   * metaMemberSigKey (this function)  vs  typeRefSigText — MetaType vs the
//     imported ParameterSymbol's `resolvedDeclType`, at the ctor-body and
//     method-body attachment loops.
//
// Obligation (b) is why parameter NAMES are not rendered inside a function type
// (§27.1, v0.306: a name is decoration — its own table spells every function
// type without one — and metaTypeToSourceText omits it from the stub, so the
// AST side could never see it) while parameter MODIFIERS are (§27.1: "`(T) ->
// ()` and `(owned T) -> ()` are distinct function types", and the modifier
// string round-trips verbatim through the wire, the stub source and
// Loader::convert).
//
// #4521 (§14/#705, INVESTIGATED, no code change): the "typeRefSigText renders
// declaration->name — the simple name" claim two lines up is only true when
// `tr->declaration` is non-null. Its Named arm ALSO has an `importedName`
// fallback for an UNRESOLVED reference, and that fallback can be the
// QUALIFIED §11.6 name (`Loader::convert` sets it from `mt.namedName`
// verbatim) where this function's Named arm always reduces to the SIMPLE
// name via `metaNamedSimpleName`. See typeRefSigTextCore's Named case for the
// full analysis of why this asymmetry cannot be observed today.
static std::string metaTypeSigTextCore(const MetaType& mt);

// The nullability-appending wrapper (fix #4516). Every recursive use below —
// generic arguments, function parameters and returns, an inline array's
// element — goes through THIS function, so a nested `Box<int?>` and a nested
// `((int) -> int)?` render their `?` exactly as a top-level one does, on all
// three sides.
static std::string metaTypeSigText(const MetaType& mt) {
    std::string s = metaTypeSigTextCore(mt);
    if (mt.nullable && mt.kind != TypeKind::InlineArray) s += '?';
    return s;
}

static std::string metaTypeSigTextCore(const MetaType& mt) {
    if (mt.kind == TypeKind::Primitive) {
        switch (mt.primitive) {
            case PrimitiveKind::Bool:    return "bool";
            case PrimitiveKind::Char:    return "char";
            case PrimitiveKind::String:  return "string";
            case PrimitiveKind::Byte:    return "byte";
            case PrimitiveKind::SByte:   return "sbyte";
            case PrimitiveKind::Short:   return "short";
            case PrimitiveKind::UShort:  return "ushort";
            case PrimitiveKind::Int:     return "int";
            case PrimitiveKind::UInt:    return "uint";
            case PrimitiveKind::Long:    return "long";
            case PrimitiveKind::ULong:   return "ulong";
            case PrimitiveKind::Float:   return "float";
            case PrimitiveKind::Double:  return "double";
            case PrimitiveKind::Decimal: return "decimal";
            case PrimitiveKind::Object:  return "Object";
            default:                     return "?";
        }
    }
    if (mt.kind == TypeKind::Named) {
        std::string s = metaNamedSimpleName(mt);  // fix #3540 — see the helper
        if (!mt.genericArgs.empty()) {
            s += '<';
            for (std::size_t i = 0; i < mt.genericArgs.size(); ++i) {
                if (i) s += ',';
                s += metaTypeSigText(mt.genericArgs[i]);
            }
            s += '>';
        }
        // The nullable `?` is appended by the `metaTypeSigText` wrapper.
        return s;
    }

    // fix #4508 (§3.1 / §15.1 / §27.1 / §22.6, v0.306): render the three
    // remaining SPELLABLE kinds instead of collapsing them onto one "?".
    //
    // THE DEFECT. Unit, Function and InlineArray all keyed "?", so §22.6
    // overloads differing ONLY at such a parameter — legal declarations; §27.1
    // makes `(int) -> int` and `(bool) -> int` distinct types, §15.1 makes
    // `[2]int` a type of its own — were indistinguishable to the §14/#705
    // matcher. The attachment loops then bound the exporter's body to the
    // wrong imported overload (`importedMethods[key] = …` is an assignment,
    // and the ctor/method loops `break` on the first key match), which is a
    // SILENT cross-module wrong value: a call the exporter compiles to one
    // answer returns the other overload's. Unit additionally MISMATCHED, since
    // both mirrors already render it "()" — so a unit-typed parameter stopped
    // ctor-body attachment outright and the importer rejected the program.
    if (mt.kind == TypeKind::Unit) return "()";

    // §27.1 / A.5 `FunctionType ::= "(" (FuncParamType …)? ")" "->" "owned"?
    // Type`; children[0..n-1] are the parameters and children[n] the return
    // (ModuleMetadata.hpp). Modifiers in, names out — see the header.
    if (mt.kind == TypeKind::Function) {
        const std::size_t n = mt.children.empty() ? 0 : mt.children.size() - 1;
        std::string s = "(";
        for (std::size_t i = 0; i < n; ++i) {
            if (i) s += ',';
            if (i < mt.funcParamMods.size() && !mt.funcParamMods[i].empty())
                s += mt.funcParamMods[i] + " ";
            s += metaTypeSigText(mt.children[i]);
        }
        s += ")->";
        if (mt.funcReturnOwned) s += "owned ";
        s += mt.children.empty() ? "?" : metaTypeSigText(mt.children[n]);
        return s;
    }

    // §15.1 / A.5 `InlineArrayType ::= "[" ConstIntExpr "owned"? "]" Type`.
    // The extent decodes through the ONE shared rule so this side and
    // typeRefSigText's `arraySizeValue` (which Loader::convert produces from
    // the same text through the same helper) cannot drift; an extent the
    // exporter could not fold renders "?" on BOTH, keeping them in lockstep
    // rather than agreeing on a wrong number. The element mirrors convert()'s
    // own `children.front()` read for the same reason. A.5 gives an inline
    // array no nullable form, so there is none to omit.
    if (mt.kind == TypeKind::InlineArray) {
        const long long n = metaInlineArrayExtent(mt.arraySize);
        std::string s = "[";
        s += n > 0 ? std::to_string(n) : "?";
        if (mt.arrayOwned) s += " owned";
        s += ']';
        s += mt.children.empty() ? "?" : metaTypeSigText(mt.children.front());
        return s;
    }

    return "?";  // Error: the sentinel kind has no canonical spelling
}

// §14/#705: canonical signature key for a MetaMember method or constructor:
// "name(type0,type1,...)" where typeN = metaTypeSigText(params[i].type).
// Used as the map key in importedMethods / ctor-matching to distinguish
// same-name overloads (§22.6 / §22.7).
static std::string metaMemberSigKey(std::string_view name,
                                    const std::vector<MetaParam>& params) {
    std::string key(name);
    key += '(';
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) key += ',';
        key += metaTypeSigText(params[i].type);
    }
    key += ')';
    return key;
}

// §14/#705: canonical type text from an ast::Type node produced by re-parsing
// a synthetic stub.  Mirrors metaTypeSigText so that a fresh MethodSymbol's
// param type produces the same key as the MetaParam it was generated from —
// see metaTypeSigText's header for the two obligations both sides discharge.
// A shape with no canonical spelling returns "?" (a no-match sentinel).
static std::string astTypeToSigTextCore(const ast::Type* t);

// The nullability-appending wrapper (fix #4516) — the AST mirror of
// `metaTypeSigText`'s. `ast::Type::nullable` is the `?` suffix the parser read
// back out of `metaTypeToSourceText`'s output, which now spells it on every
// kind that has a nullable form (its PRIMITIVE arm was the gap this fix
// closes). A `null` node has no flag to read and stays the no-match sentinel.
static std::string astTypeToSigText(const ast::Type* t) {
    std::string s = astTypeToSigTextCore(t);
    if (t && t->nullable && t->kind != ast::TypeKind::InlineArray) s += '?';
    return s;
}

static std::string astTypeToSigTextCore(const ast::Type* t) {
    if (!t) return "?";
    if (t->kind == ast::TypeKind::Unit) return "()";

    // fix #4508 (§27.1 / §15.1 / A.5, v0.306): the mirrors of
    // metaTypeSigText's two new arms. The stub this AST was parsed from is
    // `metaTypeToSourceText`'s output for the very MetaType the other side
    // renders, so the two agree spelling for spelling: modifiers rendered
    // (§27.1 identity), names not (metaTypeToSourceText omits them), nullable
    // not (both sides omit it), generic args joined with a bare ','.
    if (t->kind == ast::TypeKind::Function) {
        std::string s = "(";
        for (std::size_t i = 0; i < t->funcParams.size(); ++i) {
            if (i) s += ',';
            if (!t->funcParams[i].modifier.empty())
                s += std::string(t->funcParams[i].modifier) + " ";
            s += astTypeToSigText(t->funcParams[i].type.get());
        }
        s += ")->";
        if (t->funcReturnOwned) s += "owned ";
        s += astTypeToSigText(t->funcReturn.get());
        return s;
    }
    if (t->kind == ast::TypeKind::InlineArray) {
        // The stub spells the extent as the folded decimal the wire carried
        // (#3183), so decoding the literal's own lexeme through the SHARED
        // rule reproduces metaTypeSigText's number exactly. Anything else — a
        // non-literal extent, which a stub never produces — renders "?" on
        // this side, the same no-match sentinel the other side uses for an
        // extent it could not fold.
        const auto* lit =
            dynamic_cast<const ast::IntLiteralExpr*>(t->arraySize.get());
        const long long n = lit ? metaInlineArrayExtent(lit->text) : -1;
        std::string s = "[";
        s += n > 0 ? std::to_string(n) : "?";
        if (t->arrayOwned) s += " owned";
        s += ']';
        s += astTypeToSigText(t->arrayElement.get());
        return s;
    }
    if (t->kind == ast::TypeKind::Named && t->name &&
        !t->name->parts.empty()) {
        // In Bezel's grammar, primitive keywords (int, bool, string, …) parse
        // as a single-part NamedType, so parts.back() gives the same spelling
        // that metaTypeSigText uses.
        std::string s;
        for (std::size_t i = 0; i < t->name->parts.size(); ++i) {
            if (i) s += '.';
            s += std::string(t->name->parts[i]);
        }
        if (!t->genericArgs.empty()) {
            s += '<';
            for (std::size_t i = 0; i < t->genericArgs.size(); ++i) {
                if (i) s += ',';
                s += astTypeToSigText(t->genericArgs[i].get());
            }
            s += '>';
        }
        // The nullable `?` is appended by the `astTypeToSigText` wrapper.
        return s;
    }
    return "?";
}

// §14/#705: canonical signature key from a fresh re-parsed MethodSymbol.
// Uses astTypeToSigText for each param's declType (the AST type from the
// synthetic stub).  Must produce the same key as metaMemberSigKey for the
// MetaMember that generated the stub, so the importedMethods lookup succeeds.
static std::string freshMethodSigKey(const sema::MethodSymbol* ms) {
    std::string key(ms->name);
    key += '(';
    for (std::size_t i = 0; i < ms->params.size(); ++i) {
        if (i) key += ',';
        const auto* ps = dynamic_cast<const sema::ParameterSymbol*>(ms->params[i]);
        key += astTypeToSigText(ps ? ps->declType : nullptr);
    }
    key += ')';
    return key;
}

// §14/#705: canonical type text from a reconstructed TypeRef (an imported
// ParameterSymbol's resolvedDeclType).  Mirrors metaTypeSigText so the same
// signature key is produced for a MetaParam and its corresponding
// ParameterSymbol (used for ctor-body and method-body attachment matching in
// facet b) — see metaTypeSigText's header for the two obligations. This side's
// input is `Loader::convert(MetaType)`, a field-for-field structural copy of
// the very MetaType the other side renders, so the two stay in lockstep by
// construction (fix #4508).
static std::string typeRefSigTextCore(const sema::TypeRef* tr);

// The nullability-appending wrapper (fix #4516) — the TypeRef mirror.
// `Loader::convert` copies `nullable` field-for-field off the very MetaType
// `metaTypeSigText` renders (`ref->nullable = mt.nullable;`), so the two sides
// read ONE flag and cannot drift.
static std::string typeRefSigText(const sema::TypeRef* tr) {
    std::string s = typeRefSigTextCore(tr);
    if (tr && tr->nullable && tr->kind != sema::TypeRef::Kind::InlineArray)
        s += '?';
    return s;
}

static std::string typeRefSigTextCore(const sema::TypeRef* tr) {
    if (!tr) return "?";
    switch (tr->kind) {
        case sema::TypeRef::Kind::Unit:
            return "()";
        case sema::TypeRef::Kind::Primitive: {
            using P = sema::PrimitiveType;
            switch (tr->primitive) {
                case P::Bool:    return "bool";
                case P::Char:    return "char";
                case P::String:  return "string";
                case P::Byte:    return "byte";
                case P::SByte:   return "sbyte";
                case P::Short:   return "short";
                case P::UShort:  return "ushort";
                case P::Int:     return "int";
                case P::UInt:    return "uint";
                case P::Long:    return "long";
                case P::ULong:   return "ulong";
                case P::Float:   return "float";
                case P::Double:  return "double";
                case P::Decimal: return "decimal";
                case P::Object:  return "Object";
                default:         return "?";
            }
        }
        case sema::TypeRef::Kind::Named: {
            // #4521 (§14/#705, INVESTIGATED, no code change — see the issue's
            // own review comment for the full write-up): the two arms below
            // spell a §11.6 NESTED type DIFFERENTLY. `tr->declaration` resolved
            // renders the SIMPLE name (`declaration->name`), matching
            // metaTypeSigTextCore's `metaNamedSimpleName` reduction — the two
            // sides agree whenever resolution succeeds, by construction. The
            // `importedName` fallback below is reached only when `tr` is
            // UNRESOLVED, and `Loader::convert`'s Named arm sets it to
            // `mt.namedName` VERBATIM — the QUALIFIED wire name — whenever
            // `ModuleMetadataBuilder::convertType` could not resolve the type
            // at the EXPORTER (its Named arm's `else` branch: the reference's
            // first AST name part, never dotted, is what reaches an
            // unresolved case there instead — so a dotted `importedName` can
            // ONLY originate from an exporter-side resolution, contradicting
            // "this side is unresolved"). That contradiction is exactly why
            // the drift cannot be observed today: reaching it needs a §11.6
            // type the EXPORTER resolved and the IMPORTER cannot (the type's
            // declaring module outside the consumer's import closure) — and
            // that state is refused at codegen with a diagnostic naming
            // #3577 (§2.8.3 rule 3) before any key built from this fallback
            // is ever compared. Reducing this fallback to a simple name (to
            // match the resolved arm unconditionally) would not fix anything
            // observable and would turn a harmless MISMATCH into a possible
            // COLLISION in that same unreachable state — see the issue's
            // measured §4 for why that trade was declined. If EITHER
            // precondition changes (the #3577 refusal softens, or an
            // unresolved dotted name becomes reachable some other way), this
            // pair must be revisited together — never one side alone,
            // exactly as #4508 moved all three renderers in one commit.
            std::string s;
            if (tr->declaration)
                s = std::string(tr->declaration->name);
            else if (!tr->importedName.empty())
                s = std::string(tr->importedName);
            else
                return "?";
            if (!tr->genericArgs.empty()) {
                s += '<';
                for (std::size_t i = 0; i < tr->genericArgs.size(); ++i) {
                    if (i) s += ',';
                    s += typeRefSigText(tr->genericArgs[i]);
                }
                s += '>';
            }
            // The nullable `?` is appended by the `typeRefSigText` wrapper.
            return s;
        }
        // fix #4508 (§27.1 / §15.1, v0.306): the third mirror. `funcParams`,
        // `funcReturnOwned`, `arrayOwned` and `arraySizeValue` are all copied
        // verbatim from the MetaType by Loader::convert (the extent through
        // the same `metaInlineArrayExtent` rule), so rendering them in the
        // same order with the same punctuation is byte-identical to
        // metaTypeSigText for every type both can see.
        case sema::TypeRef::Kind::Function: {
            std::string s = "(";
            for (std::size_t i = 0; i < tr->funcParams.size(); ++i) {
                if (i) s += ',';
                if (!tr->funcParams[i].modifier.empty())
                    s += std::string(tr->funcParams[i].modifier) + " ";
                s += typeRefSigText(tr->funcParams[i].type);
            }
            s += ")->";
            if (tr->funcReturnOwned) s += "owned ";
            s += typeRefSigText(tr->funcReturn);
            return s;
        }
        case sema::TypeRef::Kind::InlineArray: {
            std::string s = "[";
            s += tr->arraySizeValue > 0 ? std::to_string(tr->arraySizeValue)
                                        : "?";
            if (tr->arrayOwned) s += " owned";
            s += ']';
            s += typeRefSigText(tr->arrayElement);
            return s;
        }
        default:
            return "?";  // Error: the sentinel kind has no canonical spelling
    }
}

// §14/#705 (facet b): true when convertMember (in the Loader struct) returns
// a non-null sema::Symbol* for a given MetaMember.  Used to sync-walk
// td.members and importedType.members in pass-3 helper functions.
static bool metaMemberProducesSymbol(const MetaMember& mm) {
    if (mm.kind == MetaMemberKind::NestedType) return mm.nestedType != nullptr;
    switch (mm.kind) {
        case MetaMemberKind::Method:
        case MetaMemberKind::Field:
        case MetaMemberKind::Const:
        case MetaMemberKind::EnumConstant:
        case MetaMemberKind::Constructor:
        case MetaMemberKind::Property:
        // fix #2414/#2407: convertMember now reconstructs a symbol for these
        // two kinds too (see its Indexer/Event cases) — this predicate MUST
        // track convertMember's real return-null/non-null behavior exactly,
        // since every call site here sync-walks td.members against
        // importedType.members position-by-position (see the sites' own
        // comments) and a stale `false` here would desync that walk by one
        // for any type with a reconstructed indexer or event.
        case MetaMemberKind::Indexer:
        case MetaMemberKind::Event:
            return true;
        default:
            return false;  // any future member kind convertMember defers
    }
}

// Render MetaVisibility as a Bezel visibility keyword.
static std::string metaVisibilityToKeyword(MetaVisibility v) {
    switch (v) {
        case MetaVisibility::Public:    return "public";
        case MetaVisibility::Private:   return "private";
        case MetaVisibility::Protected: return "protected";
        case MetaVisibility::Internal:  return "internal";
        default:                        return "public";
    }
}

// fix #2842 (§14.1/§2.5/#783, v0.240): per-STUB skip, paralleling the #2820
// shape. The synthetic single-class source declares only the type being
// reconstructed — sibling types of the exporting module are not in scope
// (UNLESS the source begins with `import <moduleName>;`, which is only
// buildSyntheticFieldInitSource's path — see #2351/#2375). A method stub
// whose SIGNATURE (return type or any parameter type) names a sibling Named
// type therefore fails at resolveTypes of the fresh context — and that
// failure is reported by the SAME error channel as the body being
// reconstructed. parseAndAttachMethodBody's #783 soundness gate (and its
// ctor/dtor siblings) then refuses to attach the BODY under reconstruction
// for the WHOLE class, even though the defect is in one unrelated sibling's
// stub. Today the whole-class disable surfaces as a clean REJECTED compile
// via the §14.1/#2821 layout gate; before #2821 it was a silent wrong value.
//
// The fix is the per-stub equivalent of #2820's per-stub shape. Skip a stub
// whose signature references a Named type other than the enclosing class
// (which IS declared) — the stub is omitted from the synthetic source, so the
// fresh context's resolveTypes succeeds for the remaining members. A body
// that referenced the skipped sibling would fail at typecheckExpressions (the
// resolution target does not exist) and parseAndAttachMethodBody correctly
// declines to attach THAT body — the OTHER methods of the class still
// reconstruct, which is exactly the per-stub granularity this fix aims at.
//
// The ctor/dtor-body paths are TOLERANT of post-typecheck sema errors (a
// base(...) chain on an unknown base is benign — only the field/param/method
// remap matters there), so a body that CALLED the skipped sibling would be
// silently attached with the call dropped — a #2290-bug-2-shaped wrong value.
// The parse-and-attach functions therefore mirror the helper: if ANY method
// stub was skipped, the ctor/dtor body is NOT attached (return false), so the
// §14.1/#783/#2821 gate stays in force for that body. The method-body path's
// existing soundness gate covers its own case.
//
// buildSyntheticFieldInitSource declares `import <moduleName>;` (#2375), so
// sibling types ARE visible in its synthetic context — the helper is told so
// via the `importsModule` flag, and the per-stub skip is suppressed on that
// path (an existing test surface depends on the import-resolved form).
//
// InScope names: the enclosing class (`selfName`) AND its generic-parameter
// names — both are bound by the synthetic source's class header (`class
// Box<T>` declares both `Box` and `T`). A method signature like `Get(): T`
// uses the type parameter `T` and is therefore safe — without this carve-out
// the check would spuriously skip every method that references a type
// parameter of its own class (regression-measured against the #783 test
// surface — xmod_generic_method_783's T1–T3 all use `Box<T>` with a
// `Get(): T` member, and treating `T` as "non-self Named" leaves the class
// with no methods to reconstruct).
static bool metaTypeReferencesNonSelfNamed(
    const MetaType& mt,
    const std::string& selfName,
    const std::unordered_set<std::string>& inScopeGenericParams) {
    switch (mt.kind) {
        case TypeKind::Primitive:
        case TypeKind::Error:
        case TypeKind::Unit:
            return false;
        case TypeKind::Named: {
            // A Named type that names the enclosing class IS declared in the
            // synthetic source, so it is safe; a Named type that names a
            // generic parameter (`T`) is bound by the class header, so it is
            // also safe. Anything else (a sibling type like `Cell16`) is not
            // declared and the stub must be skipped. We recurse through
            // generic args (`Box<Cell16>` is not safe even though `Box` is).
            for (const MetaType& arg : mt.genericArgs) {
                if (metaTypeReferencesNonSelfNamed(arg, selfName,
                                                   inScopeGenericParams))
                    return true;
            }
            // fix #3540 (§11.6, v0.285): compare the SIMPLE name — `selfName`
            // is `MetaTypeDecl::name` and the synthetic source's class header
            // declares that identifier, not a qualified path. See
            // `metaNamedSimpleName`.
            const std::string simpleName = metaNamedSimpleName(mt);
            if (simpleName.empty()) return false;
            if (simpleName == selfName) return false;
            if (inScopeGenericParams.count(simpleName)) return false;
            return true;
        }
        case TypeKind::InlineArray:
            // InlineArray's element type is `children[0]`. Empty `children` is
            // the unresolved sentinel — treat as safe.
            if (mt.children.empty()) return false;
            return metaTypeReferencesNonSelfNamed(mt.children[0], selfName,
                                                  inScopeGenericParams);
        case TypeKind::Function: {
            // Function's children are params followed by the return type
            // (mirrors ModuleMetadata.hpp's schema comment on MetaType).
            for (const MetaType& c : mt.children) {
                if (metaTypeReferencesNonSelfNamed(c, selfName,
                                                   inScopeGenericParams))
                    return true;
            }
            return false;
        }
    }
    return false;
}

// True iff emitting a stub for `mm` in a synthetic source whose enclosing
// class is `td.name` would not reference any Named type that source does not
// declare. False (skip the stub) iff the signature names another Named type
// of the exporting module.
static bool methodSignatureIsStubSafe(const MetaMember& mm,
                                      const MetaTypeDecl& td) {
    std::unordered_set<std::string> inScopeGenericParams;
    for (const auto& gp : td.genericParams) {
        if (!gp.name.empty()) inScopeGenericParams.insert(gp.name);
    }
    // fix #2842 (gap fill — found at per-fix review): also include THIS
    // method's own generic-parameter names. The synthetic stub declares them
    // as part of `mm`'s own header (`<U>` in `method Bar<U>(x: U): U`,
    // emitted by buildMethodStub's "Generic type params (method-level `<T>`)"
    // arm below). Without this they would be misread as "non-self Named" by
    // metaTypeReferencesNonSelfNamed — the stub would be incorrectly skipped,
    // and any sibling method whose body references `Bar` would itself become
    // unattached when the synthetic source fails to resolve the call. No
    // existing test exercises the shape, but the carve-out is structural and
    // matches what the stub actually emits on the wire.
    for (const auto& gp : mm.genericParams) {
        if (!gp.name.empty()) inScopeGenericParams.insert(gp.name);
    }
    const std::string& selfName = td.name;
    if (metaTypeReferencesNonSelfNamed(mm.type, selfName,
                                       inScopeGenericParams)) return false;
    for (const MetaParam& p : mm.params) {
        if (metaTypeReferencesNonSelfNamed(p.type, selfName,
                                           inScopeGenericParams)) return false;
    }
    return true;
}

// §14/#662c / #695: build a stub declaration for a method whose body is not
// available.  The stub only carries the signature — enough for sema to register
// a MethodSymbol so a self-call inside the body being reconstructed (a ctor
// body, a dtor body, a static-field initializer, or the #783 method body)
// resolves to it and can be remapped to the imported MethodSymbol.  Abstract
// methods have no body and are skipped; operators are skipped by the callers.
//
// fix #2820 (spec §14.1/§2.5, v0.240): the stub body used to be `=> 0` for EVERY
// return type (only `bool` was special-cased, and the computed `retText` was
// never used).  `0` does not typecheck against `string`, a class, `owned C`, an
// interface, `Task<…>`, `[N]T`, `() -> R` — nor, per §5.1, against `byte`,
// `sbyte`, `short`, `ushort`, `uint`, `ulong` or `char`, which reject an `int`
// literal outright.  A stub that fails to typecheck makes the fresh context
// report an error, and parseAndAttachMethodBody's #783 soundness gate then
// refuses to attach THE BODY BEING RECONSTRUCTED — even though the failure is
// entirely in an unrelated sibling's synthetic stub.  Declaring one
// reference-returning method anywhere in an imported generic class therefore
// silently disabled §14.1/#783 monomorphization for EVERY method of that class,
// on a condition no author can see; via the fallback to the exporter's
// placeholder-compiled function that could then become a wrong value (#2821).
//
// A per-return-type literal table is the wrong shape (the enumerate-the-
// categories mistake of the #2787/#2790 family — several types have no literal
// at all).  Emit instead a body that is type-correct BY CONSTRUCTION for any
// signature: a SELF-CALL forwarding the stub's own parameters, whose type IS the
// declared return type whatever that type is.  The stub is never lowered —
// parseAndAttach* runs collectSymbols / resolveTypes / typecheckExpressions on
// the synthetic module and nothing else — so the recursion is never emitted, let
// alone executed; it exists only to give sema a well-typed body.  A BARE call
// (not `this.`) is used so one spelling covers static and instance members
// alike, and because a `this.`-rooted call inside an `async` stub takes a
// different receiver-recovery path.
//
// fix #2842 (§14.1/§2.5/#783, v0.240): per-stub skip when the SIGNATURE
// references a sibling Named type AND the synthetic source does not import the
// exporting module (so the sibling type is undeclared). The stub is OMITTED
// entirely (returns an empty string) — the caller-side parse-and-attach
// function mirrors the predicate (so the ctor/dtor body paths stay sound
// against a silently-dropped self-call to the omitted sibling). The target
// method whose body IS being reconstructed is rendered verbatim from its
// methodBodyText, never from this helper, so it is not affected by the skip.
// `importsModule` is true for buildSyntheticFieldInitSource (its `import
// <moduleName>;` line makes the sibling types visible — see #2375), and false
// for every other builder (which omit the import — see the ctor/dtor/method
// header comments there). `td` provides the enclosing-class name AND the
// class's own generic-parameter names (both are bound by the synthetic
// class header) so the per-stub check recognizes `T` and friends as in scope.
// fix #4514 (§22.12 / §15.1 / §14.1/#783 / §2.8.3 rule 3, v0.306): emit the
// class-level `const` members of `td` into a synthetic reconstruction source.
//
// WHY THIS WAS MISSING AND WHAT IT COST. Every synthetic source below declares
// the class's FIELDS, PROPERTIES and METHOD STUBS so that a body (or signature)
// carried across the wire VERBATIM can resolve the names it mentions. A
// class-level `const` was never among them — and a `const` is nameable from
// exactly the same verbatim texts. The measured shape (#4514) is a `const` used
// as an INLINE-ARRAY EXTENT on a constructor parameter:
//
//     public class MxC<T>
//       public const MAX: int := 2;
//       public constructor(a: [MAX]int) … end constructor;
//
// `ctorBodyText` carries that declaration verbatim, `MAX` resolved to nothing in
// the fresh context, and — because §15.1's extent expression is NOT visited by
// ExprTC (see the `findConstShiftDomainFaultInScope` comment in
// TypeResolution_Names.cpp) — the failure was SILENT: no diagnostic, no
// `semaDiag->hasErrors()`, so the §14.1/#783 soundness gate saw a clean body and
// ATTACHED a constructor decl whose parameter's `arraySizeValue` stayed at -1.
// Codegen's `lowerTypeRef` then answered the bare `i32` placeholder for that
// parameter and `buildOrderedArgs`' §15.4 inline-array arm declined without
// producing a value — a SIGSEGV in the importing compile (the other half of
// #4514). §2.8.3 rule 3 requires the artifact interface to carry enough to
// reconstruct every public signature; `[MAX]int` is part of one.
//
// NARROWED to a PRIMITIVE-typed const with a non-empty, non-`default(...)`
// `valueText`, deliberately. `valueText` is `renderConstExpr`'s output, which is
// a re-parsable source lexeme for every literal arm (the lexer keeps the quotes
// on a string/char literal, so the text round-trips) and the empty string for
// anything it cannot render. `default(T)` is the one arm that renders a NAME —
// `de->type->name->parts.back()`, unqualified — and a name this synthetic source
// does not declare would fail to resolve and take the WHOLE reconstruction down,
// turning a gap into a regression. A primitive const is also the only kind
// §15.1 admits as an extent. Anything outside that set keeps exactly its
// pre-fix behaviour: no stub, and the same silent non-resolution.
static std::string buildConstStubs(const MetaTypeDecl& td) {
    std::string s;
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Const) continue;
        if (mm.name.empty() || mm.valueText.empty()) continue;
        if (mm.type.kind != TypeKind::Primitive) continue;
        if (mm.valueText.rfind("default(", 0) == 0) continue;
        // §22.12: a class-level `const` is implicitly static, and the parser
        // requires explicit visibility on one. `ConstMemberSymbol` carries no
        // modifiers (SymbolCollection sets none), so there are none to render.
        s += "  " + metaVisibilityToKeyword(mm.visibility) + " const " +
             mm.name + " : " + metaTypeToSourceText(mm.type) + " := " +
             mm.valueText + ";\n";
    }
    return s;
}

static std::string buildMethodStub(const MetaMember& mm,
                                   const MetaTypeDecl& td,
                                   bool importsModule) {
    std::string s;
    // Visibility and non-body modifiers (skip abstract — it suppresses the body).
    bool isAbstract = false;
    for (const std::string& mod : mm.modifiers)
        if (mod == "abstract") isAbstract = true;
    if (isAbstract) return s;  // abstract methods have no body; skip stub
    // fix #2842: per-stub skip when the signature names a sibling type that the
    // synthetic source does not declare. Returning the empty string here is the
    // per-stub equivalent of the `isAbstract` arm above. Suppressed when the
    // source imports the module — see header comment.
    if (!importsModule && !methodSignatureIsStubSafe(mm, td)) return s;
    s += "  " + metaVisibilityToKeyword(mm.visibility);
    // Emit only the modifiers the parser accepts before `method`.
    for (const std::string& mod : mm.modifiers) {
        if (mod == "virtual" || mod == "override" || mod == "sealed" ||
            mod == "static" || mod == "async" || mod == "new")
            s += " " + mod;
        // Skip "readonly" (§22.8) — not a method modifier in Bezel syntax.
        // Skip "abstract" — guarded above.
    }
    s += " method ";
    s += std::string(mm.name.empty() ? "unknown" : mm.name);
    // Generic type params (method-level `<T>`).
    if (!mm.genericParams.empty()) {
        s += "<";
        for (std::size_t i = 0; i < mm.genericParams.size(); ++i) {
            if (i) s += ", ";
            s += mm.genericParams[i].name;
        }
        s += ">";
    }
    s += "(";
    for (std::size_t i = 0; i < mm.params.size(); ++i) {
        if (i) s += ", ";
        s += mm.params[i].name + ": " + metaTypeToSourceText(mm.params[i].type);
    }
    s += ")";
    // Return type (omit for void / Unit).
    bool hasReturnType = mm.type.kind != TypeKind::Unit &&
                         mm.type.kind != TypeKind::Error;
    if (hasReturnType) s += ": " + metaTypeToSourceText(mm.type);
    // fix #2820: an expression body that is well-typed for ANY return type — a
    // self-call forwarding this stub's own parameters (see the header comment).
    if (hasReturnType) {
        s += " => " + std::string(mm.name.empty() ? "unknown" : mm.name) + "(";
        for (std::size_t i = 0; i < mm.params.size(); ++i) {
            if (i) s += ", ";
            s += mm.params[i].name;
        }
        s += ");";
    } else {
        s += " end " + std::string(mm.name.empty() ? "unknown" : mm.name) + ";";
    }
    return s;
}

// fix #2954 (§22.8 "Field initializers" / §2.8.3 rule 4 / §14.1 "Generic
// declarations" / §2.5, v0.240): REBIND every TypeRef in a re-parsed body that
// resolved to the SYNTHETIC reconstruction's own class so it names the IMPORTED
// TypeSymbol instead.
//
// THE SIXTH SIDE TABLE, AND THE ONE WHOSE ENTRIES ARE WRONG RATHER THAN MISSING.
// All four re-parse paths below copy `allResolvedTypes()` / `allExprTypes()` into
// mainCtx verbatim (#2855's fix added the last of them). Verbatim is right for
// every TypeRef naming an outside type — but a TypeRef naming the class being
// reconstructed points at the FRESH TypeSymbol collected from
// `buildSyntheticClassSource` / `buildSyntheticMethodSource`, and that symbol is
// a DIFFERENT TYPE from the imported one in every way codegen cares about:
//
//   * its fields carry NO initializers — those functions emit each field as
//     `name : Type;`, never `:= <fieldInitText>` — so §22.8's "Field
//     initializers run as part of construction in textual declaration order" had
//     nothing to run;
//   * `ctx_.isImported()` is FALSE for it, so `lowerNew` takes neither the
//     imported-generic branch nor the local branch and NO VTABLE is installed;
//   * `classStruct` mints it a SECOND `%class.<Name>$spec0`, so one
//     monomorphization has two LLVM struct types and two identities — flatly
//     against §2.8.3 rule 4's "one runtime type identity ... regardless of which
//     artifact produced the value".
//
// MEASURED on this fix's parent, `new GRe10<T>()` inside an imported generic's
// own re-lowered `Nested()` body (the issue's repro): `w=21` beside
// `nested=21` — the outer construction ran the initializer, the inner one read
// `inner._w` as 0, so the sum was 21 where 42 is correct. The emitted IR for the
// inner `new` is a bare `calloc` with no vtable store and no `store i32 21`,
// against the outer's calloc + `store ptr @"vt$GRe10$spec0"` + `store i32 21`.
//
// #2954 REQUIRES THIS DIRECTION IN AS MANY WORDS — "whatever runs the
// initializers has to be the SAME sequence the importing module's own `new
// GRe10<string>()` runs, not a second reconstruction of it". Teaching the
// synthetic source to re-emit each field's initializer would have been the
// second reconstruction, and would still have left the type identity split.
// Rebinding the reference makes the inner `new` reach the very same `lowerNew`
// path as the outer one, so the two spellings cannot diverge again.
//
// The walk is over the whole TypeRef GRAPH, not just the top level, because the
// self type can appear nested (`new Box<GRe10<T>>()`, a `[4]GRe10<T>` element, a
// `(GRe10<T>) -> int` parameter). `seen` is required for termination: TypeRefs
// are arena-owned and a self-referential generic argument would otherwise
// recurse forever.
static void rebindSelfTypeRefs(sema::TypeRef* ref,
                               const sema::TypeSymbol* freshType,
                               sema::TypeSymbol* importedType,
                               std::unordered_set<sema::TypeRef*>& seen) {
    if (!ref || !freshType || !importedType) return;
    if (!seen.insert(ref).second) return;
    if (ref->declaration == freshType) ref->declaration = importedType;
    for (sema::TypeRef* a : ref->genericArgs)
        rebindSelfTypeRefs(a, freshType, importedType, seen);
    rebindSelfTypeRefs(ref->arrayElement, freshType, importedType, seen);
    rebindSelfTypeRefs(ref->funcReturn, freshType, importedType, seen);
    for (const auto& fp : ref->funcParams)
        rebindSelfTypeRefs(fp.type, freshType, importedType, seen);
}

// fix #2954: the whole-table form — run over both type side tables of a fresh
// re-parse context before they are copied into mainCtx. Every one of the four
// reconstruction paths calls this, because the defect is a property of the
// reconstruction (a synthetic self-class), not of any one member kind.
static void rebindSelfTypeRefsIn(sema::SemaContext& freshCtx,
                                 const sema::TypeSymbol* freshType,
                                 sema::TypeSymbol* importedType) {
    std::unordered_set<sema::TypeRef*> seen;
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        rebindSelfTypeRefs(ref, freshType, importedType, seen);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        rebindSelfTypeRefs(ref, freshType, importedType, seen);
}

// Build a synthetic Bezel source string for a mini class containing only the
// imported fields, stub method declarations (for self-call resolution), and the
// given ctor body text.  Used to re-parse + sema-check the ctor body with proper
// symbol bindings so its AST exprs are registered in the fresh SemaContext.
// §14/#662c / #695: method stubs are required so that self-method calls inside
// the ctor body (e.g. `computed := computeConst()`) can be resolved to a fresh
// MethodSymbol by sema, which is then remapped to the imported MethodSymbol by
// the caller.  Example output:
//   module _662c_reparsed_;
//   public class Box<T>
//     private _slot : T;
//     private method computeConst(): int => 0;
//     public constructor(v : T) _slot := v; end constructor;
//   end Box;
static std::string buildSyntheticClassSource(
    const MetaTypeDecl& td, const std::string& ctorBodyText) {
    std::string src = "module _662c_reparsed_;\n";
    // Record constructors must be reconstructed as records: buildRecordValue
    // binds `this` to the by-value storage and inlines this declaration.
    src += metaVisibilityToKeyword(td.visibility);
    src += " ";
    src += (td.kind == MetaTypeDeclKind::Record ? "record " : "class ");
    src += td.name;
    if (!td.genericParams.empty()) {
        src += "<";
        for (std::size_t i = 0; i < td.genericParams.size(); ++i) {
            if (i) src += ", ";
            // Variance prefix (in/out) if present.
            if (!td.genericParams[i].variance.empty() &&
                td.genericParams[i].variance != "none") {
                src += td.genericParams[i].variance + " ";
            }
            src += td.genericParams[i].name;
        }
        src += ">";
    }
    src += "\n";
    // fix #4514 (§22.12 / §15.1 / §2.8.3 rule 3, v0.306): class-level `const`
    // declarations, so a VERBATIM text this source carries can name one — see
    // buildConstStubs for the measured `[MAX]int` constructor-parameter shape.
    src += buildConstStubs(td);
    // Fields (needed so implicit field references resolve).
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Field) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " " + mm.name + " : " + metaTypeToSourceText(mm.type) + ";\n";
    }
    // fix #3207 (§9.2 / §2.5, v0.254): property STUBS so an AUTHORED ctor
    // body that names a sibling body-storage member (`this.X := v;` where
    // `X` is an auto-property) can resolve `this.X` in the fresh context.
    // Without a stub, the synthetic BodyRec has no `X` member, `this.X` is
    // rejected at sema with "no member `X` on type `BodyRec`", and the body's
    // PropertySymbol binding is never created — so the remap loop has nothing
    // to remap, the body silently keeps no field-store symbol, and codegen's
    // storageMemberOf resolves the access against the wrong struct
    // (`fieldIndex_` is keyed on the imported type, the symbol is keyed on
    // the synthetic re-parse type — two distinct PropertySymbols whose
    // indices coincide only by luck, and the store silently lands at the
    // wrong address). Mirrors the Field stub above: a one-line
    // `public [readonly] property NAME: TYPE;` declaration per MetaMember of
    // kind Property (the same wire bit #2478 reads on the importer side). A
    // computed property's accessor bodies are NOT carried — re-parsing
    // accessor bodies in the synthetic single-type context fails on
    // sibling-type references (#710/#2290) — but the bare `property NAME: T;`
    // declaration is enough for a ctor body to NAME the property; reads
    // dispatch through `importedPropGetterFns_` / `-Setters` (the existing
    // cross-module path), writes are rejected by the read-only modifier
    // upstream, exactly as for any read-only imported property.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Property) continue;
        if (mm.type == MetaType{}) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " property " + mm.name + " : " + metaTypeToSourceText(mm.type);
        if (!mm.valueText.empty())
            src += " := " + mm.valueText;
        src += ";\n";
    }
    // §14/#662c / #695: stub method declarations so self-calls in the ctor body
    // resolve to a MethodSymbol in the fresh context (remapped to the imported
    // MethodSymbol by parseAndAttachCtorBody).  Operator methods are skipped —
    // they use a different declaration syntax; they are unlikely to be called
    // explicitly from a ctor body and do not need stub resolution.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Method) continue;
        if (mm.isOperator) continue;
        std::string stub = buildMethodStub(mm, td, /*importsModule=*/false);
        if (!stub.empty()) src += stub + "\n";
    }
    // The ctor body (verbatim from the exporter's span extraction).
    src += "  " + ctorBodyText + "\n";
    src += "end " + td.name + ";\n";
    return src;
}

// Parse ctorBodyText for a constructor of an imported generic class, run the
// three sema passes in a FRESH SemaContext, then walk the fresh exprSymbols to
// build remapped entries in the MAIN context (mainCtx):
//   fresh IdentifierExpr* → fresh FieldSymbol   →  mainCtx[same expr*] = importedFieldSym
//   fresh IdentifierExpr* → fresh ParamSymbol   →  mainCtx[same expr*] = importedParamSym
//   fresh IdentifierExpr* → fresh LocalSymbol   →  mainCtx[same expr*] = SAME freshSym
//     (#2290 bug 1: a `var`/`using var` LOCAL declared inside the ctor body has
//     no imported counterpart to translate to — it is private to this body — so
//     it is registered by IDENTITY. This only works if `freshSym` (and the
//     TypeRefs it and the body's expressions carry) stay alive through codegen;
//     see the ownImportedAuxiliary keep-alive below, mirroring #783's
//     parseAndAttachMethodBody.)
// Attaches the re-parsed ast::ConstructorDecl to `importedCs->decl` and keeps
// the fresh SourceFile alive via mainCtx.ownImportedAst().
// Returns false if parsing or sema failed (caller should leave decl=null).
static bool parseAndAttachCtorBody(
    const MetaTypeDecl& td,
    const std::string& ctorBodyText,
    sema::ConstructorSymbol* importedCs,
    const sema::TypeSymbol& importedType,
    sema::SemaContext& mainCtx) {
    // fix #2842 (§14.1/§2.5, v0.240): if any sibling method's signature names a
    // type the synthetic source does not declare, that method's stub is skipped
    // (so resolveTypes succeeds for the class). A ctor body that CALLED the
    // skipped sibling would now silently fail at typecheckExpressions — and the
    // ctor path tolerates that error, so the body would be attached with the
    // call DROPPED (a #2290-bug-2-shaped wrong value). Mirror the predicate
    // and decline to attach the ctor body when any stub was skipped, so the
    // §14.1/#783/#2821 gate stays in force — exactly the trade documented in
    // the #2842 issue body. parseAndAttachMethodBody does not need this check
    // because its own post-typecheck soundness gate (semaDiag->hasErrors()
    // → return false) already keeps a body that references a skipped sibling
    // from attaching.
    for (const MetaMember& mmItem : td.members) {
        if (mmItem.kind != MetaMemberKind::Method) continue;
        if (mmItem.isOperator) continue;
        if (!methodSignatureIsStubSafe(mmItem, td)) return false;
    }
    std::string source = buildSyntheticClassSource(td, ctorBodyText);

    // Fresh diagnostics/lexer/parser for the mini class.
    DiagnosticEngine freshDiag;
    Lexer freshLex(source, freshDiag);
    Parser freshParser(freshLex, freshDiag);
    auto sf = freshParser.parseCompilationUnit();
    if (!sf || freshDiag.hasErrors()) return false;

    // §14/#662c / #2290: the fresh context AND its diagnostics must OUTLIVE
    // this function, exactly as in #783's parseAndAttachMethodBody — codegen
    // monomorphizes the ctor body per instantiation and, for a LOCAL variable
    // declared inside the body (#2290 bug 1), the exprSymbol registered below
    // for its references IS the fresh LocalSymbol itself (there is no imported
    // symbol to translate to), so the fresh arena that owns it cannot be torn
    // down on return. Heap-allocate both; SemaContext holds a
    // DiagnosticEngine&, so the engine is owned first (destroyed last).
    auto semaDiag = std::make_shared<DiagnosticEngine>();
    auto freshCtxPtr = std::make_shared<sema::SemaContext>(*semaDiag);
    sema::SemaContext& freshCtx = *freshCtxPtr;
    sema::collectSymbols(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    sema::resolveTypes(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    sema::typecheckExpressions(*sf, freshCtx);
    // Note: sema may emit errors for unsupported constructs (e.g. `base(...)`
    // chain on unknown base); tolerate those — we only need the exprSymbols.
    // A hard parse failure is caught above; sema errors here are usually
    // benign for the subset we're interested in (field assign + param refs).
    // NOT tolerated: #710/#2290 sibling-type references (a call/`new` naming a
    // type this synthetic reconstruction never declared) remain a silent-wrong-
    // value risk this function does not attempt to fix — see #2290 bug 2 and
    // #710 for the (XL, declaration-rewriting) follow-up.

    // Locate the fresh TypeSymbol and ConstructorSymbol in the fresh module.
    sema::TypeSymbol* freshType = nullptr;
    if (freshCtx.module()) {
        for (sema::Symbol* child : freshCtx.module()->children) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(child);
            if (ts && std::string(ts->name) == td.name) {
                freshType = ts;
                break;
            }
        }
    }
    if (!freshType) return false;

    sema::ConstructorSymbol* freshCs = nullptr;
    for (sema::Symbol* m : freshType->members) {
        auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
        if (cs && !cs->isStatic) { freshCs = cs; break; }
    }
    if (!freshCs) return false;

    // Locate the ast::ConstructorDecl — it is the `decl` pointer of freshCs.
    auto* parsedCd = dynamic_cast<const ast::ConstructorDecl*>(freshCs->decl);
    if (!parsedCd) return false;

    // Build name→FieldSymbol map for the imported type so we can remap quickly.
    std::unordered_map<std::string_view, sema::Symbol*> importedFields;
    for (sema::Symbol* m : importedType.members) {
        if (dynamic_cast<sema::FieldSymbol*>(m) && !m->name.empty())
            importedFields[m->name] = m;
    }

    // §14/#662c / #695 / #705 (facet a): build signature-key → MethodSymbol map
    // for the imported type so self-method calls inside the ctor body remap to
    // the CORRECT overload.  The prior name-only map (issue #695) overwrote the
    // first overload when two methods shared a name (§22.6); keying by
    // name+param-types selects the right one.  We sync-walk td.members and
    // importedType.members (both in insertion order from loadMetadata pass 2) to
    // pair each MetaMember's param MetaTypes (for key-building) with the
    // corresponding imported MethodSymbol.
    std::unordered_map<std::string, sema::Symbol*> importedMethods;
    {
        std::size_t importedIdx = 0;
        for (const MetaMember& mmItem : td.members) {
            if (!metaMemberProducesSymbol(mmItem)) continue;
            if (importedIdx >= importedType.members.size()) break;
            sema::Symbol* sym = importedType.members[importedIdx++];
            if (mmItem.kind == MetaMemberKind::Method && !mmItem.isOperator) {
                importedMethods[metaMemberSigKey(mmItem.name, mmItem.params)] =
                    sym;
            }
        }
    }

    // fix #3207 (§9.2 / §2.5, v0.254): build a name→imported-PropertySymbol
    // map keyed on the wire bit `mm.isAutoProperty` (#2478). The companion of
    // the buildSyntheticClassSource property stubs above: a stub creates a
    // fresh PropertySymbol in the synthetic context; this map is what the
    // remap loop below uses to redirect every `this.X` reference (and any
    // `this[i] := v` for a property-name indexer the wire carried as
    // Property) at the fresh symbol back to the IMPORTED PropertySymbol so
    // codegen's `storageMemberOf` / `fieldIndex_` / `recordStruct` are keyed
    // on the imported type — the same struct whose `slot` is being filled by
    // `buildRecordValue`. Without this remap the fresh symbol's parent is the
    // synthetic BodyRec (a separate LLVM StructType), and `fieldAddr` builds
    // a fresh recordStruct for it on the fly; the GEP lands at the right
    // byte offset for `{i32}` storage but the store and the load use
    // DIFFERENT struct-type metadata, and LLVM's optimizer folds the
    // mismatch away (measured: `r.X` reads back 0 after `r := new BodyRec(7)`,
    // even though the ctor's `Console.WriteLine("ctor ran with " + x)` runs
    // with the right argument). Gated on `isAutoProperty` because a
    // COMPUTED property's fresh decl carries accessor bodies the existing
    // inlined-getter/setter path relies on — those bodies cannot be re-parsed
    // faithfully in the single-type synthetic context (#710/#2290), and
    // gating by `mm.isAutoProperty` is exactly the predicate #2478 already
    // ships for the layout side. A computed property in the ctor body reads
    // through `importedPropGetterFns_` (the cross-module getter stub),
    // unchanged from before this fix.
    //
    // The wire bit is read off `td.members` directly rather than
    // `mainCtx.importedPropertyIsAutoStorage_` because publish-to-context
    // happens at the END of loadMetadataPass2 and this function is called
    // mid-pass — the context side-table is not yet populated. The wire bit
    // is the same source #2478 reads from on the exporter side; reading it
    // here is the same gate, observed one function call earlier.
    std::unordered_map<std::string_view, sema::Symbol*> importedAutoPropStorage;
    {
        // fix #4650 (§9.2 "Records" / §2.5 "Cross-module symbols" /
        // §2.8.3 rule 3, v0.307): the index must advance on EVERY member that
        // produced a symbol, not only on the Property ones. This was the one
        // sync-walk in this file that gated the ADVANCE on its own member KIND
        // instead of `metaMemberProducesSymbol` — the discipline that
        // predicate's own comment spells out ("every call site here sync-walks
        // td.members against importedType.members position-by-position … a
        // stale `false` here would desync that walk by one"). `continue`-ing
        // past a non-Property member WITHOUT consuming its slot desynchronises
        // the pairing by the number of non-Property members declared BEFORE
        // the property.
        //
        // MEASURED (two-module AOT build, §9.2 body record):
        //   public record Counter
        //     private _count: int;                    -- declared FIRST
        //     public readonly property Label: string;
        //     public constructor(label: string, count: int)
        //       Label := label; _count := count;
        //     end constructor;
        //   end Counter;
        // The Field `_count` was skipped without advancing, so the Property
        // `Label` paired against `importedType.members[0]` — the FieldSymbol
        // `_count` — the `dynamic_cast<PropertySymbol*>` failed, and the map
        // came out EMPTY. The remap arm below then found no imported twin for
        // the fresh `Label`, so NO `mainCtx` exprSymbol entry was written at
        // all: codegen's `implicitFieldAddr` got a null symbol back from
        // `ctx_.exprSymbol`, `lvalueAddr` returned null, and the #4045 backstop
        // DROPPED the store while letting the compile continue — a running
        // binary whose `Label` was never initialised (empty string, or `0` for
        // the `int` spelling), then a SIGSEGV the moment §8.3 content
        // comparison or `+` dereferenced the never-stored `bz_string`.
        // Declaring the property BEFORE the field masked it exactly (index 0
        // then paired correctly), which is why the auto-property-only shape
        // (#3207's own fixture) has always worked.
        std::size_t importedIdx = 0;
        for (const MetaMember& mmItem : td.members) {
            if (!metaMemberProducesSymbol(mmItem)) continue;
            if (importedIdx >= importedType.members.size()) break;
            sema::Symbol* sym = importedType.members[importedIdx++];
            if (mmItem.kind != MetaMemberKind::Property) continue;
            auto* ps = dynamic_cast<sema::PropertySymbol*>(sym);
            if (!ps || ps->isIndexer || sym->name.empty()) continue;
            if (mmItem.isAutoProperty)
                importedAutoPropStorage[sym->name] = sym;
        }
    }

    // Build index→ParameterSymbol map for the imported ctor.
    const std::vector<sema::Symbol*>& importedParams = importedCs->params;

    // fix #2839 (§14/#662c / §27.3/§27.5, v0.240): every symbol that is a
    // LAMBDA's parameter somewhere in this ctor body — see the identical set in
    // parseAndAttachMethodBody for why the ParameterSymbol arm below needs it.
    std::unordered_set<const sema::Symbol*> freshLambdaParamSyms;
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        for (sema::Symbol* ps : syms)
            if (ps) freshLambdaParamSyms.insert(ps);

    // Walk fresh context's exprSymbols and remap into mainCtx.
    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        // fix #4514 (§22.12 "Static members" / §14.1 / §2.8.3 rule 3,
        // v0.306): a class-level `const` reference. `buildConstStubs` now
        // declares the type's consts in every synthetic reconstruction source,
        // so a VERBATIM body that names one resolves — to a symbol of the
        // FRESH context, which `mainCtx` knows nothing about. Without this arm
        // the reference is DROPPED from `mainCtx`'s exprSymbols,
        // `Emitter::constMemberValue` finds no `ConstMemberSymbol` and returns
        // null, and the enclosing expression "failed to lower" (a #2491 /
        // #2961 internal diagnostic and a silent zero). MEASURED: an imported
        // generic's `return Tag + K;` over `public const K: int := 11;`
        // answered 0 instead of 16 with the stubs in place and this arm
        // missing — the stub and the remap are one change.
        //
        // Registered BY IDENTITY, exactly as a body-local `var` is (#2290
        // bug 1), and deliberately NOT remapped to the imported
        // `ConstMemberSymbol` by name: a §22.12 const has no storage and no
        // runtime identity to unify (the library emits no global for one —
        // issue #328 FINDING [3] BUG A), so §2.8.3 rule 4's one-identity
        // obligation has nothing to bind here. What the fresh symbol DOES
        // carry is the exporter's own `ast::ConstDecl`, whose initializer is
        // the very expression `renderConstExpr` put on the wire, so
        // `constMemberValue`'s local fold answers the value the exporter
        // compiled. The fresh AST and context outlive this function already,
        // for exactly the reason the local-`var` arm needs them to
        // (ownImportedAst / ownImportedAuxiliary at the tail).
        if (dynamic_cast<sema::ConstMemberSymbol*>(freshSym)) {
            mainCtx.setExprSymbol(expr, freshSym);
        } else if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            // Field reference: find same-named field in importedType.
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (auto* freshPs = dynamic_cast<sema::PropertySymbol*>(freshSym)) {
            // fix #3207 (§9.2 / §2.5, v0.254): remap an auto-property
            // reference (a `this.X` read or `this.X := v` write) inside the
            // re-parsed body to the IMPORTED PropertySymbol. The ctor body's
            // symbols were populated by the synthetic re-parse above, where
            // `X` resolves against the SYNTHETIC BodyRec — a different
            // PropertySymbol (different `parent`, different `decl`) than the
            // one whose struct `slot` is being filled by `buildRecordValue`.
            // `isAutoPropertyStorageSymbol` (the gate above) is true for the
            // fresh symbol too — its freshly-parsed PropertyDecl has
            // bodyKind == Auto — so codegen's `storageMemberOf` accepts it as
            // storage, but `fieldIndex_` is keyed on the IMPORTED symbol and
            // the synthetic symbol is absent, so `fieldAddr` builds a fresh
            // recordStruct on the fly under the wrong struct identity and
            // the field store/load never observe each other. Remap by name to
            // the imported symbol so codegen uses the imported struct
            // throughout. See buildSyntheticClassSource's property-stub arm
            // above for the matching wire-bit side.
            auto it = importedAutoPropStorage.find(freshPs->name);
            if (it != importedAutoPropStorage.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (dynamic_cast<sema::ParameterSymbol*>(freshSym)) {
            // fix #2839: a LAMBDA parameter is declared inside the body and has
            // no importer-side counterpart (like a body-local, #2290 bug 1
            // below) — carry it over verbatim rather than running the
            // positional ctor-param search that can never match it.
            if (freshLambdaParamSyms.count(freshSym)) {
                mainCtx.setExprSymbol(expr, freshSym);
                continue;
            }
            // Param reference: find by position in freshCs.params.
            for (std::size_t pi = 0; pi < freshCs->params.size(); ++pi) {
                if (freshCs->params[pi] == freshSym &&
                    pi < importedParams.size()) {
                    mainCtx.setExprSymbol(expr, importedParams[pi]);
                    break;
                }
            }
        } else if (auto* freshMs = dynamic_cast<sema::MethodSymbol*>(freshSym)) {
            // §14/#662c / #695 / #705 (facet a): method call inside the ctor
            // body — look up by full signature key (name + param types) so the
            // correct overload is selected, not just any same-named method.
            auto it = importedMethods.find(freshMethodSigKey(freshMs));
            if (it != importedMethods.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (dynamic_cast<sema::LocalSymbol*>(freshSym)) {
            // #2290 bug 1: a LOCAL variable declared inside the ctor body
            // (`var x := …;` / `using var x := …;`) has no counterpart in
            // importedType's members — it is private to this body. Register the
            // fresh LocalSymbol itself under mainCtx (kept alive below), so
            // codegen's `ctx_.exprSymbol(expr)` finds the SAME symbol pointer at
            // both the local's declaration (inlineCtorBody processes the
            // attached ast::ConstructorDecl directly) and every later reference,
            // instead of finding nothing and silently reading a default value.
            mainCtx.setExprSymbol(expr, freshSym);
        }
        // Other symbols (TypeSymbol for type refs, etc.) are not remapped;
        // they are not needed by inlineCtorBody / implicitFieldAddr.
    }

    // #2290 bug 1 / #783 precedent: copy the body's resolved-type and
    // expr-type entries from the fresh context into mainCtx so codegen's
    // per-instantiation monomorphization finds a type for LOCAL declarations
    // and expressions the field/param/method remaps above don't cover (e.g. a
    // local's declared/inferred type, or a `new OwningBuffer<int>(...)`
    // sub-expression). The TypeRef* values point into the fresh context's
    // arena, kept alive via ownImportedAuxiliary below; the AST-node keys are
    // the re-parsed nodes, kept alive via ownImportedAst. Copying the whole map
    // is harmless — the keys are unique to the fresh AST.
    //
    // fix #2954: but NOT verbatim for a TypeRef naming the class being
    // reconstructed — that one points at the SYNTHETIC self-class, which has no
    // field initializers, no vtable and its own struct identity. Rebind it to the
    // imported TypeSymbol first (see rebindSelfTypeRefs).
    rebindSelfTypeRefsIn(freshCtx, freshType,
                         const_cast<sema::TypeSymbol*>(&importedType));
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        if (astTy && ref) mainCtx.setResolvedType(astTy, ref);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        if (expr && ref) mainCtx.setExprType(expr, ref);
    // fix #2839 (§14/#662c / §27.3 Lambda expressions, v0.240): carry the
    // body's per-lambda parameter lists too — the fourth AST-keyed side table,
    // and the only one no remap loop copied. codegen's lowerLambda reads
    // `ctx_.lambdaParams(&lam)` to declare the closure's user parameters and to
    // bind each incoming argument to its ParameterSymbol; with no entry the
    // closure took ZERO user parameters and the lambda's value was lost, so
    // `var f := lambda(x: int) => x + 1; tag := f(n);` in an imported generic
    // constructor stored 0. Verbatim copy: keys are the re-parsed
    // ast::LambdaExpr nodes, values the fresh ParameterSymbols with no
    // importer-side twin; both are kept alive below.
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        if (lam) mainCtx.setLambdaParams(lam, syms);

    // fix #2857 (§19.3/#1853 / §2.8.2 clause 2, v0.240): and the FIFTH — the
    // body's bare-name local-const `case` patterns. See the identical copy in
    // parseAndAttachMethodBody for the full reasoning; verbatim for the same
    // reason as #2818's LocalSymbols (declared inside the body, no
    // importer-side twin), with both key and value kept alive below.
    for (const auto& [pat, sym] : freshCtx.allPatternSymbols())
        if (pat && sym) mainCtx.setPatternSymbol(pat, sym);

    // Attach the parsed ConstructorDecl and keep the AST alive.
    // Keep the backing source string alive too: all string_view fields in the
    // re-parsed AST (IdentifierExpr::name, AssignmentStmt::op, …) point into
    // it — if it were destroyed on function return they would dangle.
    importedCs->decl = parsedCd;
    mainCtx.ownImportedSource(std::move(source));
    mainCtx.ownImportedAst(std::move(sf));
    // Keep the fresh context + diagnostics alive (#2290 bug 1: the LocalSymbol
    // entries registered above point into freshCtx's arena). importedAuxiliary_
    // destroys in REVERSE insertion order, and SemaContext holds a
    // DiagnosticEngine&, so push the engine FIRST (destroyed last) and the
    // context SECOND (destroyed first) — context-then-engine, never dangling.
    mainCtx.ownImportedAuxiliary(semaDiag);
    mainCtx.ownImportedAuxiliary(freshCtxPtr);
    return true;
}

// Similarly for the destructor body.
static std::string buildSyntheticDtorSource(
    const MetaTypeDecl& td, const std::string& dtorBodyText) {
    // Reuse the class-source builder but replace the ctor slot with dtor text.
    std::string src = "module _662c_reparsed_dtor_;\n";
    src += metaVisibilityToKeyword(td.visibility);
    src += " class ";
    src += td.name;
    if (!td.genericParams.empty()) {
        src += "<";
        for (std::size_t i = 0; i < td.genericParams.size(); ++i) {
            if (i) src += ", ";
            if (!td.genericParams[i].variance.empty() &&
                td.genericParams[i].variance != "none")
                src += td.genericParams[i].variance + " ";
            src += td.genericParams[i].name;
        }
        src += ">";
    }
    src += "\n";
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Field) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " " + mm.name + " : " + metaTypeToSourceText(mm.type) + ";\n";
    }
    // §14/#662c / #695: stub method declarations for self-call resolution in the
    // dtor body (same discipline as the ctor-body path in buildSyntheticClassSource).
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Method) continue;
        if (mm.isOperator) continue;
        std::string stub = buildMethodStub(mm, td, /*importsModule=*/false);
        if (!stub.empty()) src += stub + "\n";
    }
    src += "  " + dtorBodyText + "\n";
    src += "end " + td.name + ";\n";
    return src;
}

static bool parseAndAttachDtorBody(
    const MetaTypeDecl& td,
    const std::string& dtorBodyText,
    sema::TypeSymbol& importedType,
    sema::SemaContext& mainCtx) {
    // fix #2842 (§14.1/§2.5, v0.240): mirror the ctor-body predicate (see the
    // identical comment at parseAndAttachCtorBody) — the dtor path is equally
    // TOLERANT of post-typecheck sema errors, so a dtor body that referenced a
    // skipped sibling stub would be attached with the call silently dropped.
    for (const MetaMember& mmItem : td.members) {
        if (mmItem.kind != MetaMemberKind::Method) continue;
        if (mmItem.isOperator) continue;
        if (!methodSignatureIsStubSafe(mmItem, td)) return false;
    }
    std::string source = buildSyntheticDtorSource(td, dtorBodyText);

    DiagnosticEngine freshDiag;
    Lexer freshLex(source, freshDiag);
    Parser freshParser(freshLex, freshDiag);
    auto sf = freshParser.parseCompilationUnit();
    if (!sf || freshDiag.hasErrors()) return false;

    // fix #2855 (§14/#662c / §28.4, v0.240): the fresh context AND its
    // diagnostics must OUTLIVE this function, exactly as in the ctor (#2290)
    // and method (#783) paths. Until this fix nothing in mainCtx pointed into
    // freshCtx, so a stack context was safe — and the dtor body consequently
    // arrived in the importer with NO types at all, which is the crash this
    // change fixes. The four things carried below (resolved types, expr types,
    // body-local symbols, lambda parameter lists) all point into the fresh
    // arena, so it is heap-allocated and kept alive at the tail. Order matters:
    // SemaContext holds a DiagnosticEngine&, so the engine is owned FIRST
    // (destroyed last) and the context SECOND (destroyed first).
    auto semaDiagPtr = std::make_shared<DiagnosticEngine>();
    DiagnosticEngine& semaDiag = *semaDiagPtr;
    auto freshCtxPtr = std::make_shared<sema::SemaContext>(semaDiag);
    sema::SemaContext& freshCtx = *freshCtxPtr;
    sema::collectSymbols(*sf, freshCtx);
    if (semaDiag.hasErrors()) return false;
    sema::resolveTypes(*sf, freshCtx);
    sema::typecheckExpressions(*sf, freshCtx);

    // Locate fresh TypeSymbol.
    sema::TypeSymbol* freshType = nullptr;
    if (freshCtx.module()) {
        for (sema::Symbol* child : freshCtx.module()->children) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(child);
            if (ts && std::string(ts->name) == td.name) { freshType = ts; break; }
        }
    }
    if (!freshType) return false;

    // Locate fresh DestructorSymbol.
    sema::DestructorSymbol* freshDs = nullptr;
    for (sema::Symbol* m : freshType->members) {
        if (auto* ds = dynamic_cast<sema::DestructorSymbol*>(m)) { freshDs = ds; break; }
    }
    if (!freshDs) return false;

    auto* parsedDd = dynamic_cast<const ast::DestructorDecl*>(freshDs->decl);
    if (!parsedDd) return false;

    // Build field remap map.
    std::unordered_map<std::string_view, sema::Symbol*> importedFields;
    for (sema::Symbol* m : importedType.members) {
        if (dynamic_cast<sema::FieldSymbol*>(m) && !m->name.empty())
            importedFields[m->name] = m;
    }

    // §14/#662c / #695 / #705 (facet a): build signature-key → MethodSymbol map
    // for the imported type so self-method calls inside the dtor body remap to
    // the CORRECT overload (same fix as parseAndAttachCtorBody).
    std::unordered_map<std::string, sema::Symbol*> importedMethods;
    {
        std::size_t importedIdx = 0;
        for (const MetaMember& mmItem : td.members) {
            if (!metaMemberProducesSymbol(mmItem)) continue;
            if (importedIdx >= importedType.members.size()) break;
            sema::Symbol* sym = importedType.members[importedIdx++];
            if (mmItem.kind == MetaMemberKind::Method && !mmItem.isOperator) {
                importedMethods[metaMemberSigKey(mmItem.name, mmItem.params)] =
                    sym;
            }
        }
    }

    // fix #2855 (§14/#662c / §27.3/§27.5, v0.240): every symbol that is a
    // LAMBDA's parameter somewhere in this dtor body — see the identical set in
    // parseAndAttachMethodBody for why the ParameterSymbol arm below needs it.
    // (A destructor declares no parameters of its own, §28.4, so every
    // ParameterSymbol reachable from a dtor body IS a lambda's; the set is
    // still built and tested rather than assumed, so the arm stays guarded on
    // the property it actually needs rather than on the symbol's category.)
    std::unordered_set<const sema::Symbol*> freshLambdaParamSyms;
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        for (sema::Symbol* ps : syms)
            if (ps) freshLambdaParamSyms.insert(ps);

    // Remap exprSymbols.
    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        // fix #4514 (§22.12 "Static members" / §14.1 / §2.8.3 rule 3,
        // v0.306): a class-level `const` reference. `buildConstStubs` now
        // declares the type's consts in every synthetic reconstruction source,
        // so a VERBATIM body that names one resolves — to a symbol of the
        // FRESH context, which `mainCtx` knows nothing about. Without this arm
        // the reference is DROPPED from `mainCtx`'s exprSymbols,
        // `Emitter::constMemberValue` finds no `ConstMemberSymbol` and returns
        // null, and the enclosing expression "failed to lower" (a #2491 /
        // #2961 internal diagnostic and a silent zero). MEASURED: an imported
        // generic's `return Tag + K;` over `public const K: int := 11;`
        // answered 0 instead of 16 with the stubs in place and this arm
        // missing — the stub and the remap are one change.
        //
        // Registered BY IDENTITY, exactly as a body-local `var` is (#2290
        // bug 1), and deliberately NOT remapped to the imported
        // `ConstMemberSymbol` by name: a §22.12 const has no storage and no
        // runtime identity to unify (the library emits no global for one —
        // issue #328 FINDING [3] BUG A), so §2.8.3 rule 4's one-identity
        // obligation has nothing to bind here. What the fresh symbol DOES
        // carry is the exporter's own `ast::ConstDecl`, whose initializer is
        // the very expression `renderConstExpr` put on the wire, so
        // `constMemberValue`'s local fold answers the value the exporter
        // compiled. The fresh AST and context outlive this function already,
        // for exactly the reason the local-`var` arm needs them to
        // (ownImportedAst / ownImportedAuxiliary at the tail).
        if (dynamic_cast<sema::ConstMemberSymbol*>(freshSym)) {
            mainCtx.setExprSymbol(expr, freshSym);
        } else if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (auto* freshMs = dynamic_cast<sema::MethodSymbol*>(freshSym)) {
            // §14/#662c / #695 / #705 (facet a): look up by full signature key.
            auto it = importedMethods.find(freshMethodSigKey(freshMs));
            if (it != importedMethods.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (dynamic_cast<sema::LocalSymbol*>(freshSym)) {
            // fix #2855, the #2290-bug-1 / #2818 arm this path never had: a
            // symbol DECLARED INSIDE the dtor body — a `var`/`const` local, a
            // `for`/`for-in` iteration variable, a `using var` — has NO
            // importer-side counterpart to remap to. It IS the fresh symbol,
            // and its `decl` is the re-parsed ast::LocalDecl that codegen keys
            // `locals_` on. Carry the binding VERBATIM; the arena that owns it
            // is kept alive at the tail (ownImportedAuxiliary).
            mainCtx.setExprSymbol(expr, freshSym);
        } else if (dynamic_cast<sema::ParameterSymbol*>(freshSym)) {
            // fix #2855 / #2839: a LAMBDA parameter is a ParameterSymbol
            // declared inside the body, with no importer-side twin — the same
            // category as a body-local above. Carry it verbatim (its
            // ParameterSymbol is the one codegen's lowerLambda keys
            // `paramSlots_` on, from the lambdaParams entry copied below).
            // Guarded on membership in the fresh context's lambdaParams_, not
            // on the symbol's kind, so a ParameterSymbol arriving here by some
            // other route is still declined rather than silently carried.
            if (freshLambdaParamSyms.count(freshSym))
                mainCtx.setExprSymbol(expr, freshSym);
        }
    }

    // fix #2855 (§14/#662c, v0.240): carry the body's TYPE tables, which this
    // path — alone among the four reconstructions — copied NEITHER of. codegen
    // asks `ctx_.resolvedType(*declType)` / `ctx_.exprType(expr)` for every
    // declaration and sub-expression it lowers; with mainCtx holding no entry
    // for the re-parsed nodes it got a NULL TypeRef and dereferenced it —
    // `TypeRef::headName()` inside `lowerLocalDecl`, reached from
    // `emitDisposerCore` -> `emitSpecializedDisposer` ->
    // `buildImportedGenericVtable`. So ANY local declaration in an imported
    // generic class's destructor SIGSEGV'd the importing compiler with no
    // diagnostic — the lambda in #2855's repro is one instance of that, not the
    // axis: `var k: int := 5;` crashes identically.
    //
    // Verbatim copy, as in the three sibling paths and for the same reasons:
    // the keys are the re-parsed AST nodes (unique to the fresh AST, so no
    // collision with mainCtx's own) and the TypeRef* values point into the
    // fresh arena, kept alive at the tail.
    //
    // fix #2954: with the same one exception the three sibling paths make — a
    // TypeRef naming the class being reconstructed is rebound to the IMPORTED
    // TypeSymbol, not carried as the synthetic self-class.
    rebindSelfTypeRefsIn(freshCtx, freshType, &importedType);
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        if (astTy && ref) mainCtx.setResolvedType(astTy, ref);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        if (expr && ref) mainCtx.setExprType(expr, ref);
    // fix #2855 (§27.3 Lambda expressions, v0.240): and the per-lambda
    // parameter lists, the copy #2839 added to the method and ctor paths and
    // #2856 to the field-initializer path. codegen's lowerLambda reads
    // `ctx_.lambdaParams(&lam)` to declare the closure's user parameters and to
    // bind each incoming argument to its ParameterSymbol; with no entry the
    // closure takes ZERO user parameters and the lambda's value is lost.
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        if (lam) mainCtx.setLambdaParams(lam, syms);

    // fix #2857 (§19.3/#1853 / §2.8.2 clause 2, v0.240): and the FIFTH — the
    // body's bare-name local-const `case` patterns. See the identical copy in
    // parseAndAttachMethodBody for the full reasoning; verbatim for the same
    // reason as #2818's LocalSymbols (declared inside the body, no
    // importer-side twin), with both key and value kept alive below.
    for (const auto& [pat, sym] : freshCtx.allPatternSymbols())
        if (pat && sym) mainCtx.setPatternSymbol(pat, sym);

    // Create a DestructorSymbol on the imported type and attach the parsed decl.
    auto* importedDs = mainCtx.makeImportedSymbol<sema::DestructorSymbol>();
    importedDs->name = mainCtx.internString("destructor");
    importedDs->parent = &importedType;
    importedDs->scope = mainCtx.newScope();
    importedDs->decl = parsedDd;
    importedType.members.push_back(importedDs);

    // Keep the backing synthetic source string alive (string_view fields in the
    // re-parsed AST point into it) and the SourceFile itself.
    mainCtx.ownImportedSource(std::move(source));
    mainCtx.ownImportedAst(std::move(sf));
    // fix #2855: and the fresh context + diagnostics, which the four carries
    // above now point into. importedAuxiliary_ destroys in REVERSE insertion
    // order and SemaContext holds a DiagnosticEngine&, so push the engine FIRST
    // (destroyed last) and the context SECOND (destroyed first) —
    // context-then-engine, never dangling. Every `return false` above happens
    // BEFORE the first mainCtx.set*() call, so a declined reconstruction leaves
    // nothing in mainCtx pointing at the arena this drops.
    mainCtx.ownImportedAuxiliary(semaDiagPtr);
    mainCtx.ownImportedAuxiliary(freshCtxPtr);
    return true;
}

// §22.8 step 1 / §22.10 / §11.1 (#3911/#3916): build a synthetic class source
// for re-parsing a generic imported class's OWN inheritance-clause ARGUMENT
// LIST. The header carries the class with its primary-ctor parameter list
// AND its base clause (the base type comes from `td.baseTypes[0]`, the
// argument list comes from the `baseCtorArgsText` wire carriage), so the
// parser produces a fresh `ast::TypeDecl::primaryBaseCtorArgs` whose
// expressions reference positional FieldSymbols (from the primary-ctor
// params) and any other symbols that survive in the synthetic context.
//
// Field stubs are emitted for every instance field (`members` of kind Field,
// excluding the primary-ctor positional fields that the `(mp: int)` header
// itself declares as FieldSymbols) so a non-positional field reference in
// the base-clause expression resolves; method stubs are emitted for the
// same reason the ctor/dtor/method paths emit them — a base-clause argument
// that calls a sibling method must resolve in the fresh context. The
// resulting source is byte-identical in SHAPE to buildSyntheticClassSource
// (module head + class header + field/method stubs + class end) so the
// remap discipline below mirrors the ctor/dtor/method paths unchanged.
//
// `metaTypeToSourceText` is the same source-text canonicalizer the rest of
// ModuleLoader uses for sibling-type spellings (#2375/#3645); the base type
// here is the exporter's own declaration, so the spelling matches by
// construction.
static std::string buildSyntheticBaseClassSource(
    const MetaTypeDecl& td, const std::string& baseCtorArgsText) {
    std::string src = "module _3911_reparsed_base_;\n";
    // Stub declaration of the BASE TYPE so the `Base3911` name in the
    // inheritance clause resolves in the fresh module's symbol table — the
    // fresh module is a fresh empty module with no imports, and the base
    // type lives in the IMPORTED module, so the parser cannot find it by
    // name without a local stub. The stub is a one-line forward-style
    // declaration (empty body); what matters is that the base type's
    // symbol exists and the inheritance-clause `Base3911` reference binds
    // to it. The argument EXPRESSIONS are what this fix attaches to
    // `importedPrimaryBaseCtorArgs` — codegen reads those expressions and
    // does NOT call into the stub base type, so the stub's empty body is
    // sound (it never has methods to call). Only emitted when the
    // inheritance clause has a non-empty argument list (the entire point
    // of this path is the inherited-argument re-parse).
    if (!td.baseTypes.empty() && !baseCtorArgsText.empty()) {
        if (td.baseTypes[0].kind == TypeKind::Named) {
            std::string baseSimple = metaNamedSimpleName(td.baseTypes[0]);
            if (!baseSimple.empty()) {
                src += "public class " + baseSimple + " end " + baseSimple +
                       ";\n";
            }
        }
    }
    src += metaVisibilityToKeyword(td.visibility);
    src += " ";
    src += (td.kind == MetaTypeDeclKind::Record ? "record " : "class ");
    src += td.name;
    if (!td.genericParams.empty()) {
        src += "<";
        for (std::size_t i = 0; i < td.genericParams.size(); ++i) {
            if (i) src += ", ";
            if (!td.genericParams[i].variance.empty() &&
                td.genericParams[i].variance != "none") {
                src += td.genericParams[i].variance + " ";
            }
            src += td.genericParams[i].name;
        }
        src += ">";
    }
    // Primary-ctor parameter list (the `hasPositionalFields` `(mp: int)`
    // form) — emitted so the synthetic class's positional FieldSymbols
    // exist for the inheritance-clause argument expressions to reference.
    // The list itself is built by walking `td.members` for the FIRST `n`
    // FieldSymbols that are positional (the same discipline
    // `recordPositionalFieldSymbols` uses on the import side), then
    // serializing each one's `type` via `metaTypeToSourceText`. Mirrors the
    // argument-list shape `Parser_Decls.cpp:3120-323` accepts at parse time.
    std::vector<const MetaMember*> positional;
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Field) continue;
        if (!mm.isPositional) continue;
        positional.push_back(&mm);
    }
    if (!positional.empty()) {
        src += "(";
        for (std::size_t i = 0; i < positional.size(); ++i) {
            if (i) src += ", ";
            src += positional[i]->name + " : " +
                   metaTypeToSourceText(positional[i]->type);
        }
        src += ")";
    }
    // Base clause: ` : <BaseType>(<args>)` — the base type comes from
    // `td.baseTypes[0]` (the documented convention that the first entry, if
    // it is a class, is the base class; see ModuleMetadata.hpp's
    // `baseTypes` comment), and the argument list is the wire carriage
    // verbatim. The argument text is already what the exporter wrote
    // (span-extracted from `CallArgument::value->span`, joined with `", "`),
    // so re-parsing it in this synthetic context reproduces the original
    // `primaryBaseCtorArgs` shape.
    if (!td.baseTypes.empty() && !baseCtorArgsText.empty()) {
        src += " : " + metaTypeToSourceText(td.baseTypes[0]) +
               "(" + baseCtorArgsText + ")";
    }
    src += "\n";
    // Field stubs for NON-positional fields (the primary-ctor params already
    // declare their own positional FieldSymbols via the `(mp: int)` header
    // above). A base-clause argument expression that references a
    // non-positional field needs this stub so the symbol resolves in the
    // fresh context; the remap loop below redirects it at the imported
    // FieldSymbol in mainCtx.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Field) continue;
        if (mm.isPositional) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " " + mm.name + " : " + metaTypeToSourceText(mm.type) + ";\n";
    }
    // Method stubs for self-call resolution (same discipline as
    // buildSyntheticClassSource / buildSyntheticDtorSource). A base-clause
    // argument expression that calls a sibling method needs the stub.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Method) continue;
        if (mm.isOperator) continue;
        std::string stub = buildMethodStub(mm, td, /*importsModule=*/false);
        if (!stub.empty()) src += stub + "\n";
    }
    src += "end " + td.name + ";\n";
    return src;
}

// §22.8 step 1 / §22.10 / §11.1 (#3911/#3916): parse `td.baseCtorArgsText`
// (the wire carriage from the exporter) in a FRESH SemaContext with the
// synthetic class header above, extract the fresh
// `ast::TypeDecl::primaryBaseCtorArgs`, remap the symbols those expressions
// reference into the IMPORTED class's symbol table in mainCtx, and store the
// remapped `CallArgument`s on `importedType.importedPrimaryBaseCtorArgs` so
// `primaryBaseCtorArgsOf` (Symbol.hpp) reads them. Returns false (and leaves
// the field empty) when the parse fails or sema cannot bind the fresh
// symbols back to the imported ones.
//
// Symbol remap discipline mirrors the ctor-body path (`parseAndAttachCtorBody`):
//   fresh IdentifierExpr* → fresh FieldSymbol   →  mainCtx[same expr*] = importedFieldSym
//   fresh IdentifierExpr* → fresh ParamSymbol   →  mainCtx[same expr*] = importedParamSym
//   fresh IdentifierExpr* → fresh MethodSymbol  →  mainCtx[same expr*] = importedMethodSym
//   fresh IdentifierExpr* → fresh LocalSymbol   →  mainCtx[same expr*] = SAME freshSym
// … with both the fresh arena and the fresh SourceFile kept alive via
// `mainCtx.ownImported*`, exactly as the ctor/dtor/method paths do. This
// keeps the fresh `Expr*` nodes (the `CallArgument::value` unique_ptrs we
// copy into the attached vector) reachable for codegen, and keeps the
// symbol-table side entries `lowerExpr` consults populated for the same
// expr pointers.
static bool parseAndAttachBaseCtorArgs(
    const MetaTypeDecl& td,
    sema::TypeSymbol& importedType,
    sema::SemaContext& mainCtx) {
    if (td.baseCtorArgsText.empty()) return false;
    if (td.baseTypes.empty()) return false;

    std::string source = buildSyntheticBaseClassSource(td, td.baseCtorArgsText);

    // Fresh lexer/parser for the mini class with the base clause.
    DiagnosticEngine freshDiag;
    Lexer freshLex(source, freshDiag);
    Parser freshParser(freshLex, freshDiag);
    auto sf = freshParser.parseCompilationUnit();
    if (!sf) return false;
    if (freshDiag.hasErrors()) return false;

    // The fresh context AND its diagnostics must OUTLIVE this function, for
    // the same reason parseAndAttachCtorBody / parseAndAttachDtorBody / the
    // others keep their fresh contexts alive: the attached `primaryBaseCtorArgs`
    // Expr* nodes point into the fresh arena, and codegen keys its symbol
    // remaps off `ctx_.exprSymbol(expr)` which holds pointers into that arena.
    // Heap-allocate both; the engine is destroyed LAST (SemaContext holds a
    // DiagnosticEngine&), so push the engine FIRST.
    auto semaDiagPtr = std::make_shared<DiagnosticEngine>();
    DiagnosticEngine& semaDiag = *semaDiagPtr;
    auto freshCtxPtr = std::make_shared<sema::SemaContext>(semaDiag);
    sema::SemaContext& freshCtx = *freshCtxPtr;
    sema::collectSymbols(*sf, freshCtx);
    if (semaDiag.hasErrors()) return false;
    sema::resolveTypes(*sf, freshCtx);
    if (semaDiag.hasErrors()) return false;
    sema::typecheckExpressions(*sf, freshCtx);
    // Mirror the ctor-body path: tolerate post-typecheck sema errors so a
    // base-clause argument that fails a §22.5 / §5.1 check still produces
    // the fresh symbols and expr-type entries codegen needs.
    // Mirror the ctor-body path: tolerate post-typecheck sema errors so a
    // base-clause argument that fails a §22.5 / §5.1 check still produces
    // the fresh symbols and expr-type entries codegen needs. A hard parse
    // failure is caught above; sema errors here are usually benign for the
    // subset we are interested in (positional-param field reads, self-method
    // calls that the synthetic stub already gates — see #2842).

    // Locate the fresh TypeSymbol in the fresh module.
    sema::TypeSymbol* freshType = nullptr;
    if (freshCtx.module()) {
        for (sema::Symbol* child : freshCtx.module()->children) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(child);
            if (ts && std::string(ts->name) == td.name) {
                freshType = ts;
                break;
            }
        }
    }
    if (!freshType) return false;
    // SEAM-DEBT: #3911 — the freshly-parsed ModuleLoader stub is always
    // local-AST (it lives in the new module we just spawned), so the AST
    // half is safe to read here even though the canonical accessor
    // `primaryBaseCtorArgsOf` does not (it has to handle both sources).
    auto* freshTd = dynamic_cast<const ast::TypeDecl*>(freshType->decl);
    if (!freshTd) return false;
    if (freshTd->primaryBaseCtorArgs.empty()) return false;

    // Build name→FieldSymbol/ParameterSymbol/MethodSymbol maps for the
    // imported type so a fresh reference resolves to the IMPORTED symbol
    // (the ctor-body remap loop's discipline).
    std::unordered_map<std::string_view, sema::Symbol*> importedFields;
    std::unordered_map<std::string, sema::Symbol*> importedMethods;
    for (sema::Symbol* m : importedType.members) {
        if (!m || m->name.empty()) continue;
        if (dynamic_cast<sema::FieldSymbol*>(m)) {
            importedFields[m->name] = m;
        }
    }
    {
        std::size_t importedIdx = 0;
        for (const MetaMember& mmItem : td.members) {
            if (!metaMemberProducesSymbol(mmItem)) continue;
            if (importedIdx >= importedType.members.size()) break;
            sema::Symbol* sym = importedType.members[importedIdx++];
            if (mmItem.kind == MetaMemberKind::Method && !mmItem.isOperator) {
                importedMethods[metaMemberSigKey(mmItem.name, mmItem.params)] =
                    sym;
            }
        }
    }

    // Lambda-parameter symbol set — a parameter declared inside the
    // expression (rare but possible — a base-clause argument that names a
    // lambda's parameter) carries no importer-side counterpart and must be
    // carried verbatim. Same set the ctor-body / method-body / dtor-body
    // paths build for the same reason.
    std::unordered_set<const sema::Symbol*> freshLambdaParamSyms;
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        for (sema::Symbol* ps : syms)
            if (ps) freshLambdaParamSyms.insert(ps);

    // Remap fresh symbols into mainCtx. Mirrors parseAndAttachCtorBody's
    // remap loop exactly, so the ctor-body's own logic is byte-for-byte the
    // same predicate across the two paths.
    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        // fix #4514 (§22.12 "Static members" / §14.1 / §2.8.3 rule 3,
        // v0.306): a class-level `const` reference. `buildConstStubs` now
        // declares the type's consts in every synthetic reconstruction source,
        // so a VERBATIM body that names one resolves — to a symbol of the
        // FRESH context, which `mainCtx` knows nothing about. Without this arm
        // the reference is DROPPED from `mainCtx`'s exprSymbols,
        // `Emitter::constMemberValue` finds no `ConstMemberSymbol` and returns
        // null, and the enclosing expression "failed to lower" (a #2491 /
        // #2961 internal diagnostic and a silent zero). MEASURED: an imported
        // generic's `return Tag + K;` over `public const K: int := 11;`
        // answered 0 instead of 16 with the stubs in place and this arm
        // missing — the stub and the remap are one change.
        //
        // Registered BY IDENTITY, exactly as a body-local `var` is (#2290
        // bug 1), and deliberately NOT remapped to the imported
        // `ConstMemberSymbol` by name: a §22.12 const has no storage and no
        // runtime identity to unify (the library emits no global for one —
        // issue #328 FINDING [3] BUG A), so §2.8.3 rule 4's one-identity
        // obligation has nothing to bind here. What the fresh symbol DOES
        // carry is the exporter's own `ast::ConstDecl`, whose initializer is
        // the very expression `renderConstExpr` put on the wire, so
        // `constMemberValue`'s local fold answers the value the exporter
        // compiled. The fresh AST and context outlive this function already,
        // for exactly the reason the local-`var` arm needs them to
        // (ownImportedAst / ownImportedAuxiliary at the tail).
        if (dynamic_cast<sema::ConstMemberSymbol*>(freshSym)) {
            mainCtx.setExprSymbol(expr, freshSym);
        } else if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            // A base-clause argument that names a positional primary-ctor
            // parameter is the typical case (`Base(mp)` reads the fresh
            // FieldSymbol for `mp`); a non-positional field reference also
            // remaps by name.
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (auto* freshPs = dynamic_cast<sema::ParameterSymbol*>(freshSym)) {
            // A lambda parameter — carried verbatim (no importer-side
            // counterpart), guarded on membership in the fresh context's
            // lambda-param set so an unrelated ParameterSymbol is declined.
            if (freshLambdaParamSyms.count(freshSym))
                mainCtx.setExprSymbol(expr, freshSym);
        } else if (auto* freshMs = dynamic_cast<sema::MethodSymbol*>(freshSym)) {
            // A sibling-method call inside the base-clause expression — the
            // same signature-keyed remap the ctor-body path uses (#705).
            auto it = importedMethods.find(freshMethodSigKey(freshMs));
            if (it != importedMethods.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (dynamic_cast<sema::LocalSymbol*>(freshSym)) {
            // A local declared inside the base-clause expression — rare, but
            // the ctor-body / dtor-body / method-body paths all carry
            // Locals verbatim (#2290 bug 1, #2818).
            mainCtx.setExprSymbol(expr, freshSym);
        }
    }

    // Copy the fresh context's resolved-type and expr-type entries into
    // mainCtx so codegen's per-instantiation `ctx_.resolvedType(decl)` /
    // `ctx_.exprType(expr)` lookups find a TypeRef for the re-parsed nodes.
    // Verbatim copy, as the ctor/dtor/method paths do — the keys are
    // unique to the fresh AST, and the values point into the fresh arena
    // (kept alive at the tail via ownImportedAuxiliary).
    //
    // Rebind self-TypeRefs to the imported type, mirroring parseAndAttachCtorBody
    // (#2954): a TypeRef naming the class being reconstructed would point
    // at the SYNTHETIC self-class with no field initializers / vtable / its
    // own struct identity. Without the rebind, a base-clause argument that
    // names the enclosing class (`class Mid<T> { ... Mid(...) }` is not
    // valid syntax in a base clause, but the rebind is safe for the
    // generic-arg form which IS reachable — `class Mid<T> : Container<Mid>`).
    rebindSelfTypeRefsIn(freshCtx, freshType, &importedType);
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        if (astTy && ref) mainCtx.setResolvedType(astTy, ref);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        if (expr && ref) mainCtx.setExprType(expr, ref);
    // Lambda-parameter lists (carried verbatim so `lowerLambda` finds them
    // when the base-clause expression closes over a sibling lambda — rare
    // but covered for symmetry with the ctor/dtor/method paths, #2839).
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        if (lam) mainCtx.setLambdaParams(lam, syms);
    // Pattern symbols (rare in a base-clause expression, but `is` patterns
    // over local declarations would land here — copied verbatim so codegen
    // sees the same symbol the parser bound).
    for (const auto& [pat, sym] : freshCtx.allPatternSymbols())
        if (pat && sym) mainCtx.setPatternSymbol(pat, sym);

    // Attach the parsed primaryBaseCtorArgs to the IMPORTED type symbol.
    // The vector's `CallArgument::value` Expr* nodes live in the fresh arena
    // (kept alive at the tail); copying them here transfers ownership of the
    // Expr* (unique_ptr move) into the imported symbol, so the imported
    // class's `primaryBaseCtorArgsOf` reads them with the right pointer
    // identity for codegen's `ctx_.exprSymbol(expr)` side-table lookups.
    importedType.importedPrimaryBaseCtorArgs =
        std::move(const_cast<std::vector<ast::CallArgument>&>(
            freshTd->primaryBaseCtorArgs));

    // Keep the backing synthetic source string alive (string_view fields in
    // the re-parsed AST point into it), the SourceFile, and the fresh
    // SemaContext + diagnostics. Order matters: importedAuxiliary_ destroys
    // in REVERSE insertion order, and SemaContext holds a DiagnosticEngine&,
    // so push the engine FIRST (destroyed last) and the context SECOND
    // (destroyed first) — context-then-engine, never dangling.
    mainCtx.ownImportedSource(std::move(source));
    mainCtx.ownImportedAst(std::move(sf));
    mainCtx.ownImportedAuxiliary(semaDiagPtr);
    mainCtx.ownImportedAuxiliary(freshCtxPtr);
    return true;
}

// fix #4553 (§2.8.2 / §14.2, v0.306): the §14.1/#783 synthetic reconstruction
// source renders the class header's generic parameter LIST but dropped its
// `where` CONSTRAINTS, so `class Holder<T> where T: Base` came back as
// `class Holder<T>`. A body that uses a constraint-provided member
// (`Describe(item: T): string => item.Tag();`) then fails to type-resolve in
// the fresh context, the #783 soundness gate declines to attach the decl,
// `ms->decl` stays null, and codegen's HARD route at emitMethodCall falls
// through to the defining module's i32-PLACEHOLDER template — emitting
// `call ptr @"bz$Lib.Holder$Describe"(ptr, ptr)` against a `(ptr, i32)`
// declaration, which LLVM's own verifier rejects ("Call parameter type does
// not match function signature"). §2.8.2 requires an external instantiation
// at an arbitrary constraint-satisfying type argument to behave identically to
// a same-module one, so the constraint must survive the round trip.
//
// The clause can only be rendered if every TYPE constraint on it can be NAMED
// inside the single-type synthetic source. This helper answers that and, as a
// side effect, collects the exporting module's sibling `TypeSymbol`s the
// constraint names so the caller can register them in the fresh context (the
// #3645 sibling-registry mechanism, scoped here to CONSTRAINT types only).
// Scoping it that narrowly is deliberate: registering ALL public siblings would
// also admit bodies that reach an arbitrary sibling, which is the shape
// `xmod_generic_method_783`'s T5 canary deliberately keeps gated. A constraint
// is part of the generic's own public signature (§14.2) and rides the wire
// already, so admitting exactly it adds no new reconstruction guesswork.
//
// Returns false → the caller renders NO `where` clause, i.e. exactly the
// pre-fix behaviour (a constrained generic whose body does not USE the
// constraint keeps reconstructing as before).
static bool collectRenderableConstraintTypes(
    const MetaTypeDecl& td, const std::string& moduleName,
    sema::SemaContext& mainCtx,
    std::vector<sema::TypeSymbol*>& outSiblings) {
    for (const MetaGenericParam& gp : td.genericParams) {
        // A.3 `Constraint ::= "class" | "value" | "notnull" | "sendable" |
        // "hashable" | "new" "(" ")" | Type`. Whitelist rather than
        // blacklist: an unrecognised spelling would be rendered verbatim into
        // the synthetic source and fail to PARSE, which costs the whole body
        // (the §14.1/#783 gate declines and a previously-working
        // reconstruction becomes a reject). §14.4's `where T is owned` /
        // `is not owned` modifier form rides this same vector and has a
        // DIFFERENT surface syntax (`is`, not `:`), so it is excluded here by
        // simply not being on the list.
        for (const std::string& sc : gp.specialConstraints) {
            if (sc != "class" && sc != "value" && sc != "notnull" &&
                sc != "sendable" && sc != "hashable" && sc != "new")
                return false;
        }
        for (const MetaType& ct : gp.constraintTypes) {
            if (ct.kind != TypeKind::Named) return false;
            // A.3 `Constraint`'s Type alternative is not nullable, and
            // `metaTypeToSourceText` would render `Base?` — a syntax error in
            // constraint position. Decline rather than emit it.
            if (ct.nullable) return false;
            // A generic constraint naming a constructed type (`where T:
            // IComparable<U>`) needs its own argument list rendered and
            // resolved; out of scope here — decline and keep pre-fix behaviour.
            if (!ct.genericArgs.empty()) return false;
            const std::string simple = metaNamedSimpleName(ct);
            if (simple == td.name) continue;  // self-reference: declared here
            bool isOwnParam = false;
            for (const MetaGenericParam& p : td.genericParams)
                if (p.name == simple) { isOwnParam = true; break; }
            if (isOwnParam) continue;  // `where T: U` — bound by the header
            sema::TypeSymbol* found = nullptr;
            if (const std::vector<sema::TypeSymbol*>* sibs =
                    mainCtx.importedTypesOf(moduleName)) {
                for (sema::TypeSymbol* sib : *sibs) {
                    if (!sib) continue;
                    if (sib->qualifiedTypeName() == ct.namedName ||
                        std::string(sib->name) == simple) {
                        found = sib;
                        break;
                    }
                }
            }
            // Not an imported public sibling (a stdlib/builtin interface, an
            // `internal` sibling that was never exported, a third module's
            // type): it cannot be named in the synthetic source, so decline.
            if (!found) return false;
            outSiblings.push_back(found);
        }
    }
    return true;
}

// §14.1 / #783: build a synthetic class source for re-parsing ONE imported
// generic method's body.  All fields are declared (so field references resolve),
// every OTHER method is emitted as a signature-only stub (so self-calls resolve),
// and `target`'s full declaration text is emitted verbatim in place of its stub.
// The returned source is identical in shape to buildSyntheticClassSource so the
// remap discipline (field/param/self-method) carries over unchanged.
//
// fix #3645 (§9.7 / §28.9 / §2.8.3 rule 4, v0.286): the `nonGenericRecordFactoryPath`
// arm emits `import <moduleName>;` at the head of the synthetic source so a
// body that references a sibling type from the EXPORTING module (the common
// case: `Make(n: int): Pt => new Pt(n, ...)` references `Pt`, the exporting
// module's own record) resolves in the fresh context — the same cross-module
// sibling-type visibility trick buildSyntheticFieldInitSource uses (#2375).
// Without this, the body's `Pt` reference fails to resolve and the
// §14.1/#783 SOUNDNESS GATE refuses to attach the decl, defeating the
// structural fix. Generic-class bodies are unaffected — they have no
// cross-module sibling-record reference (the sibling sits in a separate
// instantiation).
static std::string buildSyntheticMethodSource(const MetaTypeDecl& td,
                                              const MetaMember& target,
                                              bool nonGenericRecordFactoryPath,
                                              const std::string& moduleName,
                                              bool renderConstraints,
                                              bool constraintNamesSibling) {
    std::string src = "module _783_reparsed_method_;\n";
    // fix #4553 (§2.8.2 / §14.2, v0.306): a rendered `where T: Base` names a
    // sibling of the EXPORTING module, so the synthetic source must import it
    // for the name to bind — the same line the #3645 arm emits. Only the
    // CONSTRAINT types are registered in the fresh context (see
    // collectRenderableConstraintTypes), so this import widens nothing else.
    if ((nonGenericRecordFactoryPath || constraintNamesSibling) &&
        !moduleName.empty()) {
        src += "import " + moduleName + ";\n";
    }
    src += metaVisibilityToKeyword(td.visibility);
    src += " class ";
    src += td.name;
    if (!td.genericParams.empty()) {
        src += "<";
        for (std::size_t i = 0; i < td.genericParams.size(); ++i) {
            if (i) src += ", ";
            if (!td.genericParams[i].variance.empty() &&
                td.genericParams[i].variance != "none")
                src += td.genericParams[i].variance + " ";
            src += td.genericParams[i].name;
        }
        src += ">";
    }
    src += "\n";
    // fix #4553 (§2.8.2 / §14.2, v0.306): the `where` clauses (A.3
    // `GenericConstraints*`, rendered after the header). Without them `T` is
    // unconstrained in the fresh context and a body that calls a
    // constraint-provided member does not resolve — see
    // collectRenderableConstraintTypes for why this is all-or-nothing.
    if (renderConstraints) {
        for (const MetaGenericParam& gp : td.genericParams) {
            if (gp.constraintTypes.empty() && gp.specialConstraints.empty())
                continue;
            src += "  where " + gp.name + ": ";
            bool firstC = true;
            for (const std::string& sc : gp.specialConstraints) {
                if (!firstC) src += ", ";
                // A.3 `Constraint ::= ... | "new" "(" ")"` — the wire carries
                // the bare `new`; the surface form takes the parens.
                src += (sc == "new" ? std::string("new()") : sc);
                firstC = false;
            }
            for (const MetaType& ct : gp.constraintTypes) {
                if (!firstC) src += ", ";
                src += metaTypeToSourceText(ct);
                firstC = false;
            }
            src += "\n";
        }
    }
    // fix #4514 (§22.12 / §15.1 / §2.8.3 rule 3, v0.306): class-level `const`
    // declarations, so a VERBATIM text this source carries can name one — see
    // buildConstStubs for the measured `[MAX]int` constructor-parameter shape.
    src += buildConstStubs(td);
    // Fields (needed so implicit field references resolve).
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Field) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " " + mm.name + " : " + metaTypeToSourceText(mm.type) + ";\n";
    }
    // fix #3994 (§14.1 / §2.5, v0.295): include PROPERTY declarations too, so
    // a method body that names an auto-property (`public method IsVNull():
    // bool => V = null;` over a `record Box<T>(public readonly property V:
    // T)`) resolves `V` in the fresh context. Without this, `V` is an
    // unresolved identifier — the §14.1/#783 soundness gate rejects the body
    // (`semaDiag->hasErrors()` true), `ms->decl` stays null, and codegen's
    // `emitInlineGenericMethod` / `buildSpecializedMethodFn` cannot monomorphize
    // the body per instantiation. The importer falls back to the library's
    // `bz$<Type>$M`, which was compiled against the unspecialized `%record.Box =
    // { i32 }` layout with T → i32 placeholder, so the `V = null` comparison
    // reads 4 bytes as i32 (the library's view) from 8 bytes of ptr storage (the
    // importer's spec0 view) — exit 1 instead of 9. Mirror the ctor-body source
    // (`buildSyntheticClassSource`, the same wire bit #2478 reads on the
    // importer side) and emit `public [readonly] property NAME: TYPE;` per
    // MetaMember of kind Property, exactly the cross-module auto-property stub
    // (the bare declaration is enough — the body is a self-call forward, never
    // reads property accessors; if it did, the same #710/#2290 sibling
    // re-parse failure the ctor-body comment notes would surface, but bodies
    // that name only own members like `=> V = null` sema-clean here).
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Property) continue;
        if (mm.type == MetaType{}) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " property " + mm.name + " : " + metaTypeToSourceText(mm.type);
        if (!mm.valueText.empty())
            src += " := " + mm.valueText;
        src += ";\n";
    }
    // fix #3226 (§25 / §14.1, v0.254): the operator body for our record-return
    // case constructs a new Vec3226<T> with named-arg syntax
    // (`new Vec3226<T>(a := x.a, b := y.b)`). The synthetic class carries fields
    // and the operator itself, but no constructor — the named-arg call then
    // trips the §22.7 soundness gate with "`new Vec3226` has 2 argument(s) but
    // the implicit default constructor takes 0" and parseAndAttachMethodBody
    // fails the typecheck gate. Declare every exported constructor as a
    // parameter-only stub so `new T(...)` resolves against a matching
    // ConstructorSymbol (the body is irrelevant — we are not calling the ctor,
    // only type-checking the operator body that references it). Skipped for
    // static ctors (they are not callable, #328 FINDING [7]).
    //
    // Records (§9.2) carry an IMPLICIT positional constructor whose parameters
    // are the record's instance fields — but that ctor is NEVER explicitly
    // declared in source, so it has NO entry in `td.members` and the loop above
    // would still leave the record without a ctor in the synthetic class.
    // Synthesize one for record types: `public constructor(<each field>: <its
    // type>) end constructor;` matches the named-arg construction
    // (`new T(a := ..., b := ...)`) the operator body uses. The instance fields
    // are exactly the members of kind Field in this kind==Record type.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Constructor) continue;
        bool isStatic = false;
        for (const std::string& mod : mm.modifiers)
            if (mod == "static") { isStatic = true; break; }
        if (isStatic) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        src += " constructor(";
        for (std::size_t pi = 0; pi < mm.params.size(); ++pi) {
            if (pi) src += ", ";
            src += mm.params[pi].name + ": " +
                   metaTypeToSourceText(mm.params[pi].type);
        }
        src += ") end constructor;\n";
    }
    if (td.kind == MetaTypeDeclKind::Record) {
        src += "  public constructor(";
        bool first = true;
        for (const MetaMember& mm : td.members) {
            if (mm.kind != MetaMemberKind::Field) continue;
            // Skip static fields — the implicit positional ctor only takes
            // instance fields (§9.2).
            bool isStatic = false;
            for (const std::string& mod : mm.modifiers)
                if (mod == "static") { isStatic = true; break; }
            if (isStatic) continue;
            if (!first) src += ", ";
            src += mm.name + ": " + metaTypeToSourceText(mm.type);
            first = false;
        }
        src += ") end constructor;\n";
    }
    // Methods: the target carries its real body; every other method is a stub.
    // fix #3226 (§25 / §14.1, v0.254): operators are now ALSO exported with
    // methodBodyText (ModuleMetadataBuilder.cpp OperatorDecl arm), so the
    // synthetic source must render the TARGET operator's full body verbatim
    // (its operator body is the body's own context for self-field / self-ctor
    // references). Non-target operators are skipped — `buildMethodStub` emits
    // `public method NAME(...)` which is NOT the operator-declaration syntax
    // (`public method operator NAME(...)`), and an operator body rarely calls
    // a sibling operator by name (the body just reads its own operands'
    // fields). Only the target needs to appear in the synthetic class for the
    // matching loop below to find the fresh MethodSymbol.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Method) continue;
        if (mm.isOperator) {
            if (&mm != &target) continue;  // skip non-target operators
            src += "  " + target.methodBodyText + "\n";
            continue;
        }
        if (&mm == &target) {
            src += "  " + target.methodBodyText + "\n";
        } else {
            std::string stub = buildMethodStub(mm, td, /*importsModule=*/false);
            if (!stub.empty()) src += stub + "\n";
        }
    }
    src += "end " + td.name + ";\n";
    return src;
}

// §14.1 / #783: parse `target.methodBodyText` for an imported generic class
// method, run the three sema passes in a FRESH SemaContext, remap the body's
// exprSymbols (field / param / self-method) into mainCtx keyed on the re-parsed
// expression pointers, and attach the re-parsed ast::MethodDecl to the imported
// MethodSymbol's `decl` slot.  After this, codegen's emitInlineGenericMethod /
// buildSpecializedMethodFn can monomorphize the body per instantiation with
// genericSubst_ active (exactly the local-generic-method path), so e.g.
// `Box<string>.Get()` returns the field at the correct `ptr` offset instead of
// the i32-placeholder offset.  Returns false (leaving decl untouched) on any
// parse/sema failure, so the §14.1/#783 sema gate stays in force for that shape.
static bool parseAndAttachMethodBody(
    const MetaTypeDecl& td,
    const MetaMember& target,
    sema::MethodSymbol* importedMs,
    const sema::TypeSymbol& importedType,
    sema::SemaContext& mainCtx,
    bool nonGenericRecordFactoryPath,
    const std::string& moduleName) {
    // fix #4553 (§2.8.2 / §14.2, v0.306): decide whether the class header's
    // `where` clauses can be reproduced, and collect the exporting module's
    // sibling types they name so they can be registered below.
    std::vector<sema::TypeSymbol*> constraintSiblings;
    const bool renderConstraints = collectRenderableConstraintTypes(
        td, moduleName, mainCtx, constraintSiblings);
    std::string source = buildSyntheticMethodSource(td, target,
        nonGenericRecordFactoryPath, moduleName, renderConstraints,
        /*constraintNamesSibling=*/!constraintSiblings.empty());

    DiagnosticEngine freshDiag;
    Lexer freshLex(source, freshDiag);
    Parser freshParser(freshLex, freshDiag);
    auto sf = freshParser.parseCompilationUnit();
    if (!sf || freshDiag.hasErrors()) {
      return false;
    }

    // §14.1 / #783: the fresh context AND its diagnostics must OUTLIVE this
    // function — codegen monomorphizes the body per instantiation and queries
    // resolvedType/exprType on the body's nodes, whose TypeRefs live in the fresh
    // context's arena. Heap-allocate both and (on success) hand them to mainCtx
    // so they stay alive through codegen. SemaContext holds a DiagnosticEngine&,
    // so the engine is owned first and outlives the context (declared first here,
    // captured in the keep-alive lambda below in reverse-destruction order).
    auto semaDiag = std::make_shared<DiagnosticEngine>();
    auto freshCtxPtr = std::make_shared<sema::SemaContext>(*semaDiag);
    sema::SemaContext& freshCtx = *freshCtxPtr;
    sema::collectSymbols(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    // fix #3645 (§9.7 / §28.9 / §2.8.3 rule 4, v0.286): register the
    // exporting module's PUBLIC sibling types (already imported into mainCtx
    // under `moduleName`) into this fresh context so the `import <moduleName>;`
    // line emitted above resolves a sibling-type reference in the reconstructed
    // method body (e.g. the body `new Pt(n, ...)` references `Pt`, a sibling
    // record of the exporting class) to the REAL imported TypeSymbol — no stub
    // declaration and no remap: the reconstructed TypeRefs/exprTypes point
    // directly at the imported symbols, exactly what codegen consumes. Mirrors
    // parseAndAttachFieldInit's #2375 sibling-registry. The reconstructed type
    // itself (td.name) is skipped so its synthetic declaration stays
    // authoritative for any self-reference (preserving pre-#3645 behavior).
    // Internal siblings are absent from mainCtx's imported registry (only
    // publics are exported, §13.2), so they still correctly fail resolveTypes
    // and reject — matching the existing §14.1/#783 soundness gate's intent.
    // The registry fires ONLY for the non-generic record-factory path so the
    // existing T5 canary (a generic method that fails to resolve a sibling)
    // still rejects: the generic-class body path does NOT receive the
    // `import <moduleName>;` line nor this sibling registration.
    if (nonGenericRecordFactoryPath && !moduleName.empty()) {
        if (const std::vector<sema::TypeSymbol*>* siblings =
                mainCtx.importedTypesOf(moduleName)) {
            for (sema::TypeSymbol* sib : *siblings) {
                if (!sib || sib->name.empty()) continue;
                if (sib->name == td.name) continue;
                freshCtx.registerImportedType(moduleName, sib);
            }
        }
    }
    // fix #4553 (§2.8.2 / §14.2, v0.306): register ONLY the sibling types the
    // rendered `where` clauses name, so `where T: Base` binds to the REAL
    // imported `Base` TypeSymbol and the body's `item.Tag()` resolves against
    // it. Deliberately narrower than the #3645 arm above — a body reaching an
    // arbitrary sibling stays gated (the T5 canary in
    // tests/module/xmod_generic_method_783_test.cpp).
    if (!moduleName.empty())
        for (sema::TypeSymbol* sib : constraintSiblings)
            if (sib && !sib->name.empty())
                freshCtx.registerImportedType(moduleName, sib);
    sema::resolveTypes(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    sema::typecheckExpressions(*sf, freshCtx);
    // §14.1 / #783 SOUNDNESS GATE: unlike the ctor-body path (which tolerates
    // benign sema errors because a ctor body is field assignments whose types
    // come from the remapped field symbols), a METHOD body's RESULT can be a
    // computed value, and the synthetic class contains ONLY this type's own
    // fields/params/methods — NOT its sibling types, module functions, or other
    // imports. A body that references any of those fails to resolve in the fresh
    // context and the unresolved call is silently DROPPED at codegen → a wrong
    // value (e.g. `var x := Helper.Compute(); return x;` returns 0, not 99). So
    // require a fully clean typecheck: any error means the body uses something
    // the importer-side reconstruction cannot faithfully reproduce → do NOT
    // attach the decl, leaving the §14.1/#783 sema gate in force (a clean reject,
    // never a wrong value). Bodies that touch only own members (the common case:
    // field reads, self-calls, params, control flow over them) sema-clean and
    // monomorphize correctly.
    if (semaDiag->hasErrors()) return false;

    // Locate the fresh TypeSymbol and the fresh target MethodSymbol (by signature
    // key — the SAME key the importedMethods remap uses, so the right overload is
    // selected).
    sema::TypeSymbol* freshType = nullptr;
    if (freshCtx.module()) {
        for (sema::Symbol* child : freshCtx.module()->children) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(child);
            if (ts && std::string(ts->name) == td.name) { freshType = ts; break; }
        }
    }
    if (!freshType) return false;

    const std::string targetKey = metaMemberSigKey(target.name, target.params);
    sema::MethodSymbol* freshMs = nullptr;
    for (sema::Symbol* m : freshType->members) {
        auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
        // fix #3226 (§25 / §14.1, v0.254): allow OPERATOR members here too.
        // The previous `!ms->isOperator` guard excluded operators, leaving
        // every imported generic-record operator's body unattached; codegen's
        // HARD route at emitMethodCall line 10449 then refused the inline
        // specialization (it required a MethodDecl on the imported symbol)
        // and the call fell through to the i32-PLACEHOLDER template, which
        // mismatched the per-instantiation record layout when the return type
        // was a record (the F6 residue of umbrella #3152). The operator's
        // name is its token (`+`, `==`, `mod`, …) — set by SymbolCollection
        // — so freshMethodSigKey produces the same `(token, params...)` key
        // as metaMemberSigKey does for the target.
        if (ms && freshMethodSigKey(ms) == targetKey) { freshMs = ms; break; }
    }
    if (!freshMs) return false;

    // fix #3226: the imported symbol's `decl` slot accepts either kind.
    // MethodDecl for plain methods, OperatorDecl for §25 operator overloads
    // (the new case). callableInfo in the codegen dispatches on either
    // (LlvmBackend_impl.hpp:2602-2614), and emitInlineGenericMethod's #1059
    // path already inlines OperatorDecl bodies.
    const ast::Node* parsedMdBase = freshMs->decl;
    if (!parsedMdBase) return false;
    if (!dynamic_cast<const ast::MethodDecl*>(parsedMdBase) &&
        !dynamic_cast<const ast::OperatorDecl*>(parsedMdBase))
        return false;

    // Field remap map.
    std::unordered_map<std::string_view, sema::Symbol*> importedFields;
    for (sema::Symbol* m : importedType.members) {
        if (dynamic_cast<sema::FieldSymbol*>(m) && !m->name.empty())
            importedFields[m->name] = m;
    }

    // Self-method remap map (signature-keyed, as in the ctor path).
    std::unordered_map<std::string, sema::Symbol*> importedMethods;
    {
        std::size_t importedIdx = 0;
        for (const MetaMember& mmItem : td.members) {
            if (!metaMemberProducesSymbol(mmItem)) continue;
            if (importedIdx >= importedType.members.size()) break;
            sema::Symbol* sym = importedType.members[importedIdx++];
            if (mmItem.kind == MetaMemberKind::Method && !mmItem.isOperator)
                importedMethods[metaMemberSigKey(mmItem.name, mmItem.params)] =
                    sym;
        }
    }

    // fix #3994 (§9.2 / §2.5, v0.295): a method body that names an auto-property
    // of `this` (`=> V = null` over `public readonly property V: T`) parses
    // against the SYNTHETIC TypeSymbol the body source declares. The fresh
    // PropertySymbol is distinct from the IMPORTED PropertySymbol, so without a
    // remap codegen's `storageMemberOf` accepts the fresh symbol (its freshly-
    // parsed PropertyDecl has bodyKind == Auto → `isAutoPropertyStorageSymbol`
    // true) but `fieldIndex_` is keyed on the imported symbol and the fresh
    // symbol is absent. The body's `V` IdentifierExpr lowers to null, the
    // surrounding `V = null` BinaryExpr lowers to null, the whole method
    // body lowers to null, and the call falls through to a no-op stub — exit
    // 1 instead of the expected 9. Mirrors the ctor-body auto-property remap
    // at line 2163 (which #3207 added for the ctor path) and reads the same
    // `mm.isAutoProperty` wire bit #2478 publishes (the importer-only context
    // side-table isn't populated yet at this mid-pass point, so the wire bit
    // is the load-bearing source). Gated on `isAutoProperty` because a
    // COMPUTED property's fresh decl carries accessor bodies the inlined
    // getter/setter path relies on; those bodies cannot be re-parsed
    // faithfully in the single-type synthetic context (#710/#2290), exactly
    // the predicate #2478 already ships.
    std::unordered_map<std::string_view, sema::Symbol*> importedAutoPropStorage;
    {
        // fix #4650 (§9.2 / §2.5 / §2.8.3 rule 3, v0.307): the identical
        // desync #3207's ctor-path twin carried — this block is a copy of it.
        // Advance on `metaMemberProducesSymbol`, exactly as this function's own
        // `importedMethods` sync-walk a few lines above does; see the ctor-path
        // copy in `parseAndAttachCtorBody` for the measured consequence. Here
        // the fallout is #3994's own failure mode rather than a dropped store:
        // with the map empty, a method body naming an auto-property declared
        // AFTER any other member keeps its fresh PropertySymbol, `fieldIndex_`
        // (keyed on the imported symbol) misses, and the body lowers to null.
        std::size_t importedIdx = 0;
        for (const MetaMember& mmItem : td.members) {
            if (!metaMemberProducesSymbol(mmItem)) continue;
            if (importedIdx >= importedType.members.size()) break;
            sema::Symbol* sym = importedType.members[importedIdx++];
            if (mmItem.kind != MetaMemberKind::Property) continue;
            auto* ps = dynamic_cast<sema::PropertySymbol*>(sym);
            if (!ps || ps->isIndexer || sym->name.empty()) continue;
            if (mmItem.isAutoProperty)
                importedAutoPropStorage[sym->name] = sym;
        }
    }

    // Param remap: the target method's own parameters (by position).
    const std::vector<sema::Symbol*>& importedParams = importedMs->params;

    // fix #2839 (§14.1/#783 / §27.3/§27.5, v0.240): every symbol that is a
    // LAMBDA's parameter somewhere in this body. A lambda parameter is a
    // ParameterSymbol, so without this set the ParameterSymbol arm below reads
    // it as "the enclosing method's parameter", finds it in NO position of
    // freshMs->params, and drops the binding. It belongs in the same category
    // as a body-local (#2818): declared INSIDE the body, no importer-side twin,
    // carried over verbatim. See the lambdaParams_ copy below for the other
    // half of this fix.
    std::unordered_set<const sema::Symbol*> freshLambdaParamSyms;
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        for (sema::Symbol* ps : syms)
            if (ps) freshLambdaParamSyms.insert(ps);

    // fix #4553 (§2.8.2 / §14.2, v0.306): the constraint types registered into
    // the fresh context above, as a set, so the MethodSymbol arm below can tell
    // "a member the CONSTRAINT provides" (already an IMPORTED symbol — carry it
    // over by identity) from "a member of some other type" (no binding, as
    // before).
    std::unordered_set<const sema::Symbol*> constraintSiblingSyms;
    for (sema::TypeSymbol* sib : constraintSiblings)
        if (sib) constraintSiblingSyms.insert(sib);

    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        // fix #4514 (§22.12 "Static members" / §14.1 / §2.8.3 rule 3,
        // v0.306): a class-level `const` reference. `buildConstStubs` now
        // declares the type's consts in every synthetic reconstruction source,
        // so a VERBATIM body that names one resolves — to a symbol of the
        // FRESH context, which `mainCtx` knows nothing about. Without this arm
        // the reference is DROPPED from `mainCtx`'s exprSymbols,
        // `Emitter::constMemberValue` finds no `ConstMemberSymbol` and returns
        // null, and the enclosing expression "failed to lower" (a #2491 /
        // #2961 internal diagnostic and a silent zero). MEASURED: an imported
        // generic's `return Tag + K;` over `public const K: int := 11;`
        // answered 0 instead of 16 with the stubs in place and this arm
        // missing — the stub and the remap are one change.
        //
        // Registered BY IDENTITY, exactly as a body-local `var` is (#2290
        // bug 1), and deliberately NOT remapped to the imported
        // `ConstMemberSymbol` by name: a §22.12 const has no storage and no
        // runtime identity to unify (the library emits no global for one —
        // issue #328 FINDING [3] BUG A), so §2.8.3 rule 4's one-identity
        // obligation has nothing to bind here. What the fresh symbol DOES
        // carry is the exporter's own `ast::ConstDecl`, whose initializer is
        // the very expression `renderConstExpr` put on the wire, so
        // `constMemberValue`'s local fold answers the value the exporter
        // compiled. The fresh AST and context outlive this function already,
        // for exactly the reason the local-`var` arm needs them to
        // (ownImportedAst / ownImportedAuxiliary at the tail).
        if (dynamic_cast<sema::ConstMemberSymbol*>(freshSym)) {
            mainCtx.setExprSymbol(expr, freshSym);
        } else if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (auto* freshPs = dynamic_cast<sema::PropertySymbol*>(freshSym)) {
            // fix #3994 (§9.2 / §2.5, v0.295): remap an auto-property
            // reference inside the re-parsed method body to the IMPORTED
            // PropertySymbol — see the importedAutoPropStorage map built
            // above for why the fresh-vs-imported symbol split is the load-
            // bearing difference (storageMemberOf would otherwise key on the
            // synthetic self-type whose struct is never built and whose
            // fieldIndex_ entry does not exist).
            auto it = importedAutoPropStorage.find(freshPs->name);
            if (it != importedAutoPropStorage.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (dynamic_cast<sema::ParameterSymbol*>(freshSym)) {
            // fix #2839: a LAMBDA parameter is not the enclosing method's — it
            // is declared inside the body and has no importer-side counterpart,
            // exactly like a body-local. Carry it over verbatim (its
            // ParameterSymbol is the one codegen's lowerLambda keys paramSlots_
            // on, from the lambdaParams entry copied below), instead of running
            // the positional method-param search that can never match it and
            // then dropping the binding.
            if (freshLambdaParamSyms.count(freshSym)) {
                mainCtx.setExprSymbol(expr, freshSym);
                continue;
            }
            for (std::size_t pi = 0; pi < freshMs->params.size(); ++pi) {
                if (freshMs->params[pi] == freshSym &&
                    pi < importedParams.size()) {
                    mainCtx.setExprSymbol(expr, importedParams[pi]);
                    break;
                }
            }
        } else if (auto* fms = dynamic_cast<sema::MethodSymbol*>(freshSym)) {
            auto it = importedMethods.find(freshMethodSigKey(fms));
            if (it != importedMethods.end()) {
                mainCtx.setExprSymbol(expr, it->second);
            } else if (fms->parent &&
                       constraintSiblingSyms.count(fms->parent)) {
                // fix #4553 (§2.8.2 / §14.2, v0.306): a call to a member the
                // CONSTRAINT provides (`item.Tag()` under `where T: Base`).
                // `importedMethods` only holds the reconstructed type's OWN
                // members, so this arm used to drop the binding: codegen then
                // saw no symbol for the callee, fell into the fieldAddr
                // member-READ fallback, and reported the #2445 "no member
                // `Tag` on substituted type `Local`" diagnostic instead of
                // emitting the call. There is nothing to remap here — the
                // constraint's type was registered into the fresh context AS
                // the imported `TypeSymbol`, so `fms` already IS the symbol
                // codegen consumes. Carry it over by identity, exactly as the
                // body-local arm below does (#2818).
                mainCtx.setExprSymbol(expr, fms);
            }
        } else if (dynamic_cast<sema::LocalSymbol*>(freshSym)) {
            // fix #2818 (§14.1 monomorphization / §2.5 modules, v0.240): a
            // symbol DECLARED INSIDE the re-parsed body — a `var`/`const`
            // local, a `for`/`for-in` iteration variable, a `using var`, a
            // pattern binding — is the one category with NO importer-side
            // counterpart to remap to. It *is* the fresh symbol: its `decl`
            // is the re-parsed `ast::LocalDecl` that codegen keys `locals_`
            // on when the body is monomorphized, and the fresh SemaContext
            // that owns it is kept alive below (ownImportedAuxiliary). Copy
            // the binding VERBATIM.
            //
            // Field/parameter/method are remapped because the importer holds
            // a different symbol for the same entity; a body-local has no
            // such twin, so "remap" degenerates to "carry over". Before this,
            // the entry was simply DROPPED: `ctx_.exprSymbol()` returned null
            // for every read of a body-local, `lowerExpr` on the identifier
            // produced no value, and the enclosing expression degraded to a
            // zero/null result (the #2491 return-path fallback) — e.g.
            // `var built := $"xm-{i}"; return built.Length;` in an imported
            // `Box<T>` returned 0 while still executing (and allocating) the
            // body. A silent wrong value across the module boundary.
            //
            // Note this hole was MASKED whenever any sibling method of the
            // same class declared a reference-typed return: buildMethodStub
            // emits `=> 0` for it, which fails to typecheck, which trips the
            // #783 soundness gate above and leaves `decl` unattached — so the
            // call fell back to the exporter's own compiled function and got
            // the right answer by accident. See #2820.
            mainCtx.setExprSymbol(expr, freshSym);
        }
    }

    // §14.1 / #783: copy the body's resolved-type and expr-type entries from the
    // fresh context into mainCtx, so codegen's per-instantiation monomorphization
    // (buildSpecializedMethodFn's return-type lowering, ReturnStmt boxing, and
    // every sub-expression's type query) finds them. The TypeRef* values point
    // into the fresh context's arena, kept alive below. The AST-node KEYS are the
    // re-parsed nodes, kept alive via ownImportedAst. Only nodes that belong to
    // the re-parsed body matter; copying the whole map is harmless because the
    // keys are unique to the fresh AST (no collision with mainCtx's own nodes).
    //
    // fix #2954 (§22.8 / §2.8.3 rule 4, v0.240): with ONE entry that must not be
    // carried verbatim — a TypeRef naming the class being reconstructed, which
    // resolves to the SYNTHETIC self-class this function's own
    // `buildSyntheticMethodSource` declared. That class has no field
    // initializers, is not `isImported`, and gets its own `classStruct`, so a
    // `new Self<T>()` inside the body allocated a vtable-less, zero-initialized
    // object of a second identity. Rebind it to the imported TypeSymbol so the
    // inner `new` takes the SAME `lowerNew` path the importing module's own
    // `new Self<Arg>()` takes. This is the direct arm of #2954's repro.
    rebindSelfTypeRefsIn(freshCtx, freshType,
                         const_cast<sema::TypeSymbol*>(&importedType));
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        if (astTy && ref) mainCtx.setResolvedType(astTy, ref);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        if (expr && ref) mainCtx.setExprType(expr, ref);
    // fix #2839 (§14.1/#783 / §27.3 Lambda expressions, v0.240): lambdaParams_
    // is the FOURTH side table this reconstruction has to carry, and it was the
    // only one nothing copied. codegen's lowerLambda reads
    // `ctx_.lambdaParams(&lam)` to (a) build the closure function's user
    // parameter type list and (b) bind each incoming argument to its
    // ParameterSymbol in paramSlots_. With no entry in mainCtx it got null, the
    // closure was declared with ZERO user parameters, and the lambda's value
    // was lost — `var f := lambda(x: int) => x + 1; return f(6);` in an
    // imported generic body returned 0, not 7, with no diagnostic. §2.8.2
    // clause 2 is the rule broken: "An external instantiation behaves
    // identically to a same-module instantiation of the same generic at the
    // same arguments" — the same source compiled as ONE module answered 7.
    //
    // Copied VERBATIM, like the two type tables above and for the same reasons:
    // the keys are the re-parsed ast::LambdaExpr nodes (unique to the fresh
    // AST, so no collision with mainCtx's own lambdas) and the values are the
    // fresh ParameterSymbols, which have no importer-side twin to remap to. The
    // fresh context that owns them is kept alive below (ownImportedAuxiliary),
    // exactly as for the TypeRefs.
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        if (lam) mainCtx.setLambdaParams(lam, syms);

    // fix #2857 (§19.3/#1853 / §2.8.2 clause 2, v0.240): patternSymbols_ is the
    // FIFTH AST-keyed side table this reconstruction has to carry, and it was
    // the last one nothing copied. sema writes it (ExprTypecheck.cpp) for
    // exactly one shape — a bare-name NamedPattern resolving to a
    // compile-time-classified local `const` used as a `case` pattern constant
    // (a Pattern is not an Expr, so exprSymbols_ cannot key it). codegen reads
    // it in constValueForPatternName to fold that const's value at the case
    // site; with no entry in mainCtx it got null and REJECTED the arm:
    //   `const K := 9; case sel of K => …` in an imported generic body failed
    //   with "the `case`/`when` pattern `K` is not supported by the backend",
    // while the same source compiled as ONE module returned 105.
    //
    // Unlike its silent siblings (#2818 every local reads 0, #2839 the lambda's
    // value is lost) this one fails LOUDLY — but it is the same §2.8.2 clause 2
    // break: "An external instantiation behaves identically to a same-module
    // instantiation of the same generic at the same arguments". A program that
    // compiles as one module must not fail through the artifact boundary.
    //
    // Copied VERBATIM like the tables above: keys are the re-parsed
    // ast::Pattern nodes (unique to the fresh AST, kept alive by
    // ownImportedAst) and the value is a symbol declared INSIDE the body, so —
    // exactly as for #2818's LocalSymbols — there is no importer-side twin to
    // remap to. The fresh context that owns it is kept alive below
    // (ownImportedAuxiliary).
    for (const auto& [pat, sym] : freshCtx.allPatternSymbols())
        if (pat && sym) mainCtx.setPatternSymbol(pat, sym);

    // Attach the parsed MethodDecl to the imported MethodSymbol and keep the
    // backing source + AST alive (string_view fields point into them), plus the
    // fresh context + diagnostics that own the copied TypeRefs.
    importedMs->decl = parsedMdBase;
    mainCtx.ownImportedSource(std::move(source));
    mainCtx.ownImportedAst(std::move(sf));
    // Keep the fresh context + diagnostics alive. importedAuxiliary_ destroys in
    // REVERSE insertion order, and SemaContext holds a DiagnosticEngine&, so push
    // the engine FIRST (destroyed last) and the context SECOND (destroyed first) —
    // context-then-engine, never a dangling reference.
    mainCtx.ownImportedAuxiliary(semaDiag);
    mainCtx.ownImportedAuxiliary(freshCtxPtr);
    return true;
}

// §14.1 / §23 / §2.8.2 / §2.8.3 rule 4 (fix #2893): build a synthetic class
// source for re-parsing ONE imported generic class's PROPERTY or INDEXER
// body text. Shape mirrors buildSyntheticMethodSource (every field is
// (re)declared with its type so field references resolve, every other
// method is emitted as a signature-only stub so self-calls resolve), but
// the TARGET member is a Property/Indexer and its body is taken from
// `target.propertyBodyText` verbatim instead of being re-emitted. Other
// properties/indexers on the same class become signature-only stubs (so
// a body that references a sibling property resolves the lookup).
static std::string buildSyntheticPropertySource(const MetaTypeDecl& td,
                                                const MetaMember& target) {
    std::string src = "module _2893_reparsed_property_;\n";
    src += metaVisibilityToKeyword(td.visibility);
    src += " ";
    // §14.1 / §23: a record and a class have the same property syntax,
    // but the synthetic declaration must use the right kind to bind
    // `this` correctly (record by-value vs. class pointer receiver,
    // §9.4 / §9.7). Mirrors buildSyntheticClassSource's record/class
    // branch exactly.
    src += (td.kind == MetaTypeDeclKind::Record ? "record " : "class ");
    src += td.name;
    if (!td.genericParams.empty()) {
        src += "<";
        for (std::size_t i = 0; i < td.genericParams.size(); ++i) {
            if (i) src += ", ";
            if (!td.genericParams[i].variance.empty() &&
                td.genericParams[i].variance != "none")
                src += td.genericParams[i].variance + " ";
            src += td.genericParams[i].name;
        }
        src += ">";
    }
    src += "\n";
    // fix #4514 (§22.12 / §15.1 / §2.8.3 rule 3, v0.306): class-level `const`
    // declarations, so a VERBATIM text this source carries can name one — see
    // buildConstStubs for the measured `[MAX]int` constructor-parameter shape.
    src += buildConstStubs(td);
    // Fields (needed so implicit field references inside the body resolve).
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Field) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " " + mm.name + " : " + metaTypeToSourceText(mm.type) + ";\n";
    }
    // Methods: signature-only stubs, so self-calls from the body resolve.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Method) continue;
        std::string stub = buildMethodStub(mm, td, /*importsModule=*/false);
        if (!stub.empty()) src += stub + "\n";
    }
    // Properties / Indexers: the TARGET carries its real body; every other
    // property/indexer is a signature-only stub so a body that references
    // a sibling property resolves the lookup.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Property &&
            mm.kind != MetaMemberKind::Indexer) continue;
        if (&mm == &target) {
            src += "  " + target.propertyBodyText + "\n";
        } else {
            // Inline property/indexer stub — mirrors
            // `buildSyntheticClassSource`'s sibling-property arm (line
            // 1589-1598): `public [readonly] property NAME: TYPE;` (auto
            // form, no body) for a Property, or `public property [params]:
            // TYPE;` for an Indexer. The body of a sibling property is
            // NOT re-parsed — only the target's body needs to lower.
            // Sibling reads dispatch through the existing
            // `importedPropGetterFns_` / `-Setters` path; writes are
            // rejected by the read-only modifier upstream.
            src += "  " + metaVisibilityToKeyword(mm.visibility);
            for (const std::string& mod : mm.modifiers) src += " " + mod;
            if (mm.kind == MetaMemberKind::Indexer) {
                src += " property [";
                for (std::size_t i = 0; i < mm.params.size(); ++i) {
                    if (i) src += ", ";
                    src += mm.params[i].name + ": " +
                           metaTypeToSourceText(mm.params[i].type);
                }
                src += "]: " + metaTypeToSourceText(mm.type);
                if (!mm.valueText.empty()) src += " := " + mm.valueText;
            } else {
                src += " property " + mm.name + " : " +
                       metaTypeToSourceText(mm.type);
                if (!mm.valueText.empty()) src += " := " + mm.valueText;
            }
            src += ";\n";
        }
    }
    src += "end " + td.name + ";\n";
    return src;
}

// §14.1 / §23 / §2.8.2 / §2.8.3 rule 4 (fix #2893): parse
// `target.propertyBodyText` for an imported generic class
// property/indexer, run the three sema passes in a FRESH SemaContext, remap
// the body's exprSymbols (field / self-method) into mainCtx keyed on the
// re-parsed expression pointers, and attach the re-parsed
// ast::PropertyDecl to the imported PropertySymbol's `decl` slot. After
// this, codegen's `computedGetterOf` / `blockGetterOf` /
// `computedSetterOf` / `blockSetterOf` / `lowerPropertyCompound` paths
// take over — monomorphizing the accessor body per instantiation with
// genericSubst_ active, exactly the local-generic-property path #2884's
// own T8 control pinned. Returns false (leaving decl untouched) on any
// parse/sema failure, so the #2884 decline stays in force for that
// shape (a clean reject, never a wrong value).
static bool parseAndAttachPropertyBody(
    const MetaTypeDecl& td,
    const MetaMember& target,
    sema::PropertySymbol* importedPs,
    const sema::TypeSymbol& importedType,
    sema::SemaContext& mainCtx) {
    std::string source = buildSyntheticPropertySource(td, target);

    DiagnosticEngine freshDiag;
    Lexer freshLex(source, freshDiag);
    Parser freshParser(freshLex, freshDiag);
    auto sf = freshParser.parseCompilationUnit();
    if (!sf || freshDiag.hasErrors()) return false;

    // §14.1 / #2893: the fresh context AND its diagnostics must OUTLIVE
    // this function — codegen monomorphizes the accessor body per
    // instantiation and queries resolvedType/exprType on the body's
    // nodes, whose TypeRefs live in the fresh context's arena.
    auto semaDiag = std::make_shared<DiagnosticEngine>();
    auto freshCtxPtr = std::make_shared<sema::SemaContext>(*semaDiag);
    sema::SemaContext& freshCtx = *freshCtxPtr;
    sema::collectSymbols(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    sema::resolveTypes(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    sema::typecheckExpressions(*sf, freshCtx);
    // §14.1 / #2893 SOUNDNESS GATE: a PROPERTY body can reference sibling
    // module functions or other imports. A body that references any of
    // those fails to resolve in the fresh context and the unresolved
    // call is silently DROPPED at codegen → a wrong value. Require a
    // fully clean typecheck: any error means the body uses something
    // the importer-side reconstruction cannot faithfully reproduce →
    // do NOT attach the decl, leaving the §14.1/#2884 sema gate in
    // force.
    if (semaDiag->hasErrors()) return false;

    // Locate the fresh TypeSymbol.
    sema::TypeSymbol* freshType = nullptr;
    if (freshCtx.module()) {
        for (sema::Symbol* child : freshCtx.module()->children) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(child);
            if (ts && std::string(ts->name) == td.name) { freshType = ts; break; }
        }
    }
    if (!freshType) return false;

    // Locate the fresh PropertySymbol (matched by NAME and isIndexer
    // flag — properties are not overloadable, §22.6).
    sema::PropertySymbol* freshPs = nullptr;
    for (sema::Symbol* m : freshType->members) {
        auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
        if (!ps) continue;
        if (std::string(ps->name) != target.name) continue;
        bool wantIndexer = (target.kind == MetaMemberKind::Indexer);
        if (ps->isIndexer != wantIndexer) continue;
        freshPs = ps;
        break;
    }
    if (!freshPs) return false;

    // The parsed declaration must be an ast::PropertyDecl.
    auto* parsedPd = dynamic_cast<const ast::PropertyDecl*>(freshPs->decl);
    if (!parsedPd) return false;

    // Field remap map.
    std::unordered_map<std::string_view, sema::Symbol*> importedFields;
    for (sema::Symbol* m : importedType.members) {
        if (dynamic_cast<sema::FieldSymbol*>(m) && !m->name.empty())
            importedFields[m->name] = m;
    }

    // Self-method remap map (signature-keyed, as in the method path).
    std::unordered_map<std::string, sema::Symbol*> importedMethods;
    {
        std::size_t importedIdx = 0;
        for (const MetaMember& mmItem : td.members) {
            if (!metaMemberProducesSymbol(mmItem)) continue;
            if (importedIdx >= importedType.members.size()) break;
            sema::Symbol* sym = importedType.members[importedIdx++];
            if (mmItem.kind == MetaMemberKind::Method && !mmItem.isOperator)
                importedMethods[metaMemberSigKey(mmItem.name, mmItem.params)] =
                    sym;
        }
    }

    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        // fix #4514 (§22.12 "Static members" / §14.1 / §2.8.3 rule 3,
        // v0.306): a class-level `const` reference. `buildConstStubs` now
        // declares the type's consts in every synthetic reconstruction source,
        // so a VERBATIM body that names one resolves — to a symbol of the
        // FRESH context, which `mainCtx` knows nothing about. Without this arm
        // the reference is DROPPED from `mainCtx`'s exprSymbols,
        // `Emitter::constMemberValue` finds no `ConstMemberSymbol` and returns
        // null, and the enclosing expression "failed to lower" (a #2491 /
        // #2961 internal diagnostic and a silent zero). MEASURED: an imported
        // generic's `return Tag + K;` over `public const K: int := 11;`
        // answered 0 instead of 16 with the stubs in place and this arm
        // missing — the stub and the remap are one change.
        //
        // Registered BY IDENTITY, exactly as a body-local `var` is (#2290
        // bug 1), and deliberately NOT remapped to the imported
        // `ConstMemberSymbol` by name: a §22.12 const has no storage and no
        // runtime identity to unify (the library emits no global for one —
        // issue #328 FINDING [3] BUG A), so §2.8.3 rule 4's one-identity
        // obligation has nothing to bind here. What the fresh symbol DOES
        // carry is the exporter's own `ast::ConstDecl`, whose initializer is
        // the very expression `renderConstExpr` put on the wire, so
        // `constMemberValue`'s local fold answers the value the exporter
        // compiled. The fresh AST and context outlive this function already,
        // for exactly the reason the local-`var` arm needs them to
        // (ownImportedAst / ownImportedAuxiliary at the tail).
        if (dynamic_cast<sema::ConstMemberSymbol*>(freshSym)) {
            mainCtx.setExprSymbol(expr, freshSym);
        } else if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (auto* fms = dynamic_cast<sema::MethodSymbol*>(freshSym)) {
            auto it = importedMethods.find(freshMethodSigKey(fms));
            if (it != importedMethods.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (dynamic_cast<sema::LocalSymbol*>(freshSym)) {
            // A `var`/`const` local, `for`/`for-in` iteration variable,
            // or `using var` declared INSIDE the accessor body has NO
            // importer-side counterpart to remap to. Copy verbatim, as
            // #2818 does for method bodies.
            mainCtx.setExprSymbol(expr, freshSym);
        } else if (dynamic_cast<sema::ParameterSymbol*>(freshSym)) {
            // A property accessor's `value` parameter (setter only) or
            // indexer's index parameter is bound by codegen's
            // lowerPropertySet / lowerIndexerGet directly; the
            // typecheck-time exprSymbol carry is harmless and rare.
            mainCtx.setExprSymbol(expr, freshSym);
        }
    }

    // Copy resolved-type and expr-type entries from the fresh context,
    // rebinding SELF references (the fresh class declaration) to the
    // imported TypeSymbol (#2954 precedent).
    rebindSelfTypeRefsIn(freshCtx, freshType,
                         const_cast<sema::TypeSymbol*>(&importedType));
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        if (astTy && ref) mainCtx.setResolvedType(astTy, ref);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        if (expr && ref) mainCtx.setExprType(expr, ref);
    // Copy the pattern-symbols side table (#2857 precedent).
    for (const auto& [pat, sym] : freshCtx.allPatternSymbols())
        if (pat && sym) mainCtx.setPatternSymbol(pat, sym);

    // Attach the parsed PropertyDecl to the imported PropertySymbol and
    // keep the backing source + AST alive (string_view fields point
    // into them), plus the fresh context + diagnostics that own the
    // copied TypeRefs.
    importedPs->decl = parsedPd;
    mainCtx.ownImportedSource(std::move(source));
    mainCtx.ownImportedAst(std::move(sf));
    // Keep the fresh context + diagnostics alive.
    mainCtx.ownImportedAuxiliary(semaDiag);
    mainCtx.ownImportedAuxiliary(freshCtxPtr);
    return true;
}

// fix #3362 (§26.3 / §26.5 step 2 / §2.8.3 rule 4, v0.NNN, importer half of
// #2583's writer-half fix): re-parse the body text of a cross-module
// EXTENSION property, in a fresh SemaContext, and attach the resulting
// `ast::PropertyDecl` to the imported PropertySymbol's `decl` slot —
// mirroring `parseAndAttachPropertyBody` above (the §14.1/#2893 generic
// class-property twin) but for an extension block. The motivation is the
// same and the only structural differences are the OUTER language shape:
//
//   (1) §26.3 admits ONLY an expression-bodied extension property
//       (`property Name: Type => expr;`) — neither the Block (accessor)
//       form nor Auto (`property X: T;` / `property X: T := v;`) — so the
//       bodyKind gate differs. The exporter's PropertyBranch already
//       enforces this on the writer side
//       (ModuleMetadataBuilder.cpp:2179 `pd->bodyKind !=
//       PropertyBodyKind::Expression`), so a non-Expression body never
//       reaches this importer either; the bodyKind check at body-attach
//       time below exists as a belt-and-braces, the same way the writer's
//       exists (the importer must not assume a shape that the writer
//       could have hand-stamped differently in some future shape).
//
//   (2) The synthetic source's OUTER form is an `extension Target`
//       block, not a `class Target` body — `me.target` gives the
//       spelled head, `target.propertyBodyText` (the serialized span,
//       written by ModuleMetadataBuilder.cpp's widened gate) gives the
//       complete `public property NAME: TYPE => EXPR;` decl. For a
//       class-declared target we also need the class declaration in the
//       source so `this._v` etc. resolves in the fresh context; for a
//       primitive target (`extension int`) the body can only use `this`,
//       and the source is just the extension block.
//
//   (3) The body reference `this._v` — the canonical extension-property
//       get pattern, per §26.3 — resolves to a FieldSymbol of the
//       FRESH (re-parsed) class declaration, not the imported one. Remap
//       FRESH FieldSymbol → imported FieldSymbol exactly the way the
//       class-property twin does (and exactly the way
//       parseAndAttachMethodBody does for §14.1 methods), so the
//       codegen-side `ps->decl` (the freshly-constructed PropertyDecl)
//       and mainCtx's exprSymbol carry-over agree on which symbol owns
//       `_v`. Without this remap the imported and fresh symbols would
//       be distinct entities with identical fields and codegen's
//       property-read lowering (`computedGetterOf` +
//       `lowerPropertyGet`) would read `_v` out of the FRESH symbol's
//       `classStruct` slot — a fresh symbol that has NO classStruct
//       registered in mainCtx — so `_v` would land on the wrong field
//       offset or fault entirely (the exact hazard #2893 / #2954
//       caused-and-fixed for the class-property twin; this importer
//       must not reintroduce the bug for the extension twin).
//
// `targetTypeSym` — the importer-side sema::TypeSymbol for the dep's
// class head, when the target is a class. May be null for a primitive
// target (no class to reference; the body can only see `this`). When
// non-null, we replicate just enough of the class shape in the synthetic
// source for the body to find its fields by name — fields only (the
// extension body never sees methods or sibling properties on `this`,
// only the named fields the getter body reads).
//
// `owningTd` — the dep's MetaTypeDecl. Used to read FIELDS for the
// synthetic source (so `this._v` resolves in the fresh context). May be
// null for a primitive target. When non-null, the synthesized class
// carries the SAME fields the dep declared, so the body's field accesses
// resolve. (We deliberately do NOT replicate methods or nested types —
// the extension body never sees them; replicating them adds parse/sema
// surface for no value.)
//
// Returns false (leaving decl untouched) on any parse/sema failure; the
// §26.5 lookupExtensionMember then silently sees an extension member of
// the wrong shape and the importer continues without the body attached
// (the property accessor reads will return 0/null/garbage — a SILENT
// wrong value, which is the regression this fix is closing — so this
// function MUST succeed whenever the body source is sound; soundness
// drift here would be a soundness loss). The §14.1/#2893 soundness-gate
// precedent (a parse/sema error means "this body used something the
// importer cannot faithfully reproduce" → do NOT attach) is restored
// for the extension twin below.
static bool parseAndAttachExtensionPropertyBody(
    const MetaExtension& me,
    const MetaTypeDecl* owningTd,
    const sema::TypeSymbol* targetTypeSym,
    const MetaMember& target,
    sema::PropertySymbol* importedPs,
    sema::SemaContext& mainCtx) {
    // (1) Build synthetic source.
    std::string src = "module _3362_reparsed_extension_property_;\n";
    if (owningTd) {
        // Replicate the target class's FIELDS only — methods/properties
        // are not visible to the extension body (this._v for a field,
        // never this.M(...) — `this` in an extension block is of the
        // target type, but the GETTER body is the property's expression,
        // which the §26.3 grammar restricts to bare expressions — a
        // sibling method call `this.Inc()` would be valid code, but is
        // not exercised by the #2583 / #3362 test surface and would
        // require the FULL class replication that `parseAndAttach
        // MethodBody` already does; out of #3362's scope per CLAUDE.md
        // §4's decline-with-reasons — file a follow-up if a real test
        // demands it).
        const std::string kindKw =
            (owningTd->kind == MetaTypeDeclKind::Record ? "record "
                                                        : "class ");
        src += metaVisibilityToKeyword(owningTd->visibility);
        src += " ";
        src += kindKw + owningTd->name + "\n";
        for (const MetaMember& mm : owningTd->members) {
            if (mm.kind != MetaMemberKind::Field) continue;
            src += "  ";
            src += metaVisibilityToKeyword(mm.visibility);
            for (const std::string& mod : mm.modifiers) src += " " + mod;
            src += " ";
            src += mm.name;
            src += " : ";
            src += metaTypeToSourceText(mm.type);
            src += ";\n";
        }
        src += "end " + owningTd->name + ";\n";
        // The extension block: head uses the class's bare name (the
        // same spelling `metaTypeToSourceText(me.target)` would emit for
        // a Named target whose declared name is `owningTd->name`).
        src += "public extension ";
        src += owningTd->name;
        src += "\n";
        src += "  ";
        src += target.propertyBodyText;
        src += "\n";
        src += "end;\n";
    } else {
        // Primitive target (e.g. `extension int { ... }`): no class
        // declaration needed. The body can only use `this` (the
        // primitive value), so a bare extension block is sufficient.
        // Head uses the same spelling the exporter's
        // `target->name->parts.back()` would have read — `metaTypeToSource
        // Text(me.target)` for a Primitive MetaType spells the keyword
        // (`int`, `string`, etc.), which matches the dep's
        // ExtensionDecl targetType.
        src += "public extension ";
        src += metaTypeToSourceText(me.target);
        src += "\n";
        src += "  ";
        src += target.propertyBodyText;
        src += "\n";
        src += "end;\n";
    }

    // (2) Parse + sema.
    DiagnosticEngine freshDiag;
    Lexer freshLex(src, freshDiag);
    Parser freshParser(freshLex, freshDiag);
    auto sf = freshParser.parseCompilationUnit();
    if (!sf || freshDiag.hasErrors()) return false;

    auto semaDiag = std::make_shared<DiagnosticEngine>();
    auto freshCtxPtr = std::make_shared<sema::SemaContext>(*semaDiag);
    sema::SemaContext& freshCtx = *freshCtxPtr;
    sema::collectSymbols(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    sema::resolveTypes(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    sema::typecheckExpressions(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;  // soundness gate (precedent: #2893)

    // (3) Locate the fresh ExtensionDecl (only one synthesized here).
    sema::ExtensionSymbol* freshExtSym = nullptr;
    const ast::ExtensionDecl* freshExtDecl = nullptr;
    if (freshCtx.module()) {
        for (sema::Symbol* child : freshCtx.module()->children) {
            auto* es = dynamic_cast<sema::ExtensionSymbol*>(child);
            if (!es) continue;
            auto* ed = dynamic_cast<const ast::ExtensionDecl*>(es->decl);
            if (!ed) continue;
            freshExtSym = es;
            freshExtDecl = ed;
            break;
        }
    }
    if (!freshExtSym || !freshExtDecl) return false;

    // (4) Find the fresh PropertySymbol by NAME (not isIndexer — §26.3
    //     extensions are never indexers).
    sema::PropertySymbol* freshPs = nullptr;
    const ast::PropertyDecl* freshPd = nullptr;
    for (sema::Symbol* m : freshExtSym->members) {
        auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
        if (!ps || ps->isIndexer) continue;
        if (std::string(ps->name) != target.name) continue;
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd) continue;
        freshPs = ps;
        freshPd = pd;
        break;
    }
    if (!freshPs || !freshPd) return false;
    // Belt-and-braces: §26.3 admits ONLY expression-bodied extension
    // properties. The exporter already enforces this; restating it on
    // the importer is a structural safety net (a future writer change
    // that admits another body kind would silently drop the body in
    // this importer — better to refuse to attach the decl in that case
    // so the gate falls through loudly).
    if (freshPd->bodyKind != ast::PropertyBodyKind::Expression) return false;

    // (5) Field remap: FRESH FieldSymbol (in the synthetic class) →
    //     imported FieldSymbol (in the dep's class). The body's
    //     `this._v` IdentifierExpr, when typechecked in freshCtx,
    //     resolves to freshPs's parent class's `_v` FieldSymbol — NOT
    //     the dep's imported `_v`. Without the remap, codegen's
    //     `ps->decl->exprBody` (the freshly-attached PropertyDecl)
    //     walks the FRESH symbol's classStruct for `_v`'s offset — a
    //     classStruct that exists only inside the fresh SemaContext
    //     and has NO backing in mainCtx. Remap so the IdentifierExpr
    //     in mainCtx points at the IMPORTED FieldSymbol, exactly the
    //     discipline parseAndAttachPropertyBody applies for the
    //     class-property twin.
    if (targetTypeSym) {
        std::unordered_map<std::string_view, sema::Symbol*> importedFields;
        for (sema::Symbol* m : targetTypeSym->members) {
            auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
            if (!fs || m->name.empty()) continue;
            importedFields[m->name] = m;
        }
        for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
            if (!expr || !freshSym) continue;
            auto* fs = dynamic_cast<sema::FieldSymbol*>(freshSym);
            if (!fs) continue;
            auto it = importedFields.find(fs->name);
            if (it != importedFields.end())
                mainCtx.setExprSymbol(expr, it->second);
        }
    }

    // (6) Copy resolved-type and expr-type entries from the fresh
    //     context, rebinding SELF references — for an extension property
    //     there is no `this` to rebind (this is the receiver type, an
    //     OWNED value passed in, not an instance of an enclosing type),
    //     but the rebinding helper is harmless to call and matches the
    //     precedent. Without targetTypeSym there is also nothing to
    //     rebind against; the rebindSelfTypeRefsIn for the class-property
    //     case is documented in #2954's fix.
    if (targetTypeSym) {
        // Find the fresh class TypeSymbol by name (only when we built
        // one — primitive-target paths have none).
        sema::TypeSymbol* freshClass = nullptr;
        if (owningTd && freshCtx.module()) {
            for (sema::Symbol* child : freshCtx.module()->children) {
                auto* ts = dynamic_cast<sema::TypeSymbol*>(child);
                if (ts && std::string(ts->name) == owningTd->name) {
                    freshClass = ts;
                    break;
                }
            }
        }
        if (freshClass) {
            rebindSelfTypeRefsIn(freshCtx, freshClass,
                                 const_cast<sema::TypeSymbol*>(targetTypeSym));
        }
    }
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        if (astTy && ref) mainCtx.setResolvedType(astTy, ref);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        if (expr && ref) mainCtx.setExprType(expr, ref);

    // (7) Attach the freshly-parsed PropertyDecl to the imported
    //     PropertySymbol. Keep the source + AST alive (string_view fields
    //     point into them), plus the fresh context + diagnostics that
    //     own the copied TypeRefs.
    importedPs->decl = freshPd;
    mainCtx.ownImportedSource(std::move(src));
    mainCtx.ownImportedAst(std::move(sf));
    mainCtx.ownImportedAuxiliary(semaDiag);
    mainCtx.ownImportedAuxiliary(freshCtxPtr);
    return true;
}

// §2.8.2 / §22.12 / §22.13 (#2351): build a synthetic class source for
// re-parsing an imported generic class's static field INITIALIZER
// expression(s). Shape mirrors buildSyntheticMethodSource: every field is
// (re)declared with its type, and — for whichever field(s) carry their own
// fieldInitText (every static field of this class the exporter carried one
// for, not just the one field this call is reconstructing right now: a
// sibling static field the initializer references then type-checks against
// its REAL initializer rather than a bare, potentially
// `readonly`-without-assignment stub; no DEFINITE-ASSIGNMENT pass runs over
// this reduced 3-pass pipeline, so a still-bare sibling would not itself
// fault, but embedding the real text is more faithful and costs nothing
// extra since the text is already on hand) — the `:=` initializer text is
// appended too. Every method is a signature-only stub (self-call
// resolution, e.g. `Hits: int := ComputeInitial();`).
//
// fix #4592 (§22.16 / §2.8.2, v0.306): `extraMembers` is spliced in verbatim
// just before `end <Type>;`. It exists so the §22.16 invariant reconstruction
// (`parseAndAttachInvariants`) can re-parse a class's `invariant(E);` members
// in the SAME synthetic context this function already builds for a field
// initializer — the two shapes have identical needs (a single expression, no
// parameters, no locals, reading the type's own storage, `const` members and
// pure self-methods), so duplicating ~90 lines of source-builder would have
// been the only alternative. Empty (the default) reproduces the pre-#4592
// source byte-for-byte for every existing caller.
static std::string buildSyntheticFieldInitSource(
    const MetaTypeDecl& td, const std::string& moduleName,
    const std::string& extraMembers = {},
    const sema::TypeSymbol* baseSym = nullptr) {
    std::string src = "module _2351_reparsed_field_;\n";
    // §2.8.2 / #2375: `import` the exporting module so its PUBLIC sibling
    // types — already loaded into the importer's mainCtx — are visible to
    // resolveTypes/typecheckExpressions here. parseAndAttachFieldInit
    // registers those siblings into this fresh context under `moduleName`;
    // the `import` is what engages Resolver's imports path (§2.2) to expose
    // them by bare name. This closes #2351's sibling-type coverage gap: a
    // static field whose declared type OR initializer references a public
    // co-declared type (e.g. `public static readonly Res: Box := new
    // Box(42);`) now reconstructs instead of being rejected. Internal
    // siblings are not in the exported metadata (§13.2: only `public`
    // top-level types are exported), so they remain unresolvable here and
    // the import still correctly rejects — the #2351 self-pass (a)
    // guarantee is preserved.
    if (!moduleName.empty()) src += "import " + moduleName + ";\n";
    // fix #4599 residual 1 (§22.16 / §11.1 / §2.8.2, v0.306): when a base
    // clause is requested and the base class declares in a DIFFERENT module
    // than `td` itself (a base reached transitively, not a same-module
    // sibling), the `import moduleName;` line above does not expose it —
    // import its own declaring module too. `baseSym` is only ever non-null
    // from `parseAndAttachInvariants` (see its header for why the base is a
    // REAL, already-loaded symbol here rather than a re-declared stub).
    if (baseSym && !baseSym->moduleName.empty() &&
        baseSym->moduleName != moduleName)
        src += "import " + baseSym->moduleName + ";\n";
    src += metaVisibilityToKeyword(td.visibility);
    src += " class ";
    src += td.name;
    if (!td.genericParams.empty()) {
        src += "<";
        for (std::size_t i = 0; i < td.genericParams.size(); ++i) {
            if (i) src += ", ";
            if (!td.genericParams[i].variance.empty() &&
                td.genericParams[i].variance != "none")
                src += td.genericParams[i].variance + " ";
            src += td.genericParams[i].name;
        }
        src += ">";
    }
    // fix #4599 residual 1: ` : Base` — a bare name, no generic argument
    // list and no constructor of its own. `baseSym` is the REAL (already
    // fully-loaded, already fully base-resolved) TypeSymbol, not text
    // reconstructed from `td.baseTypes[0]`, so there is no MetaType here to
    // render generic arguments from; a GENERIC base therefore needs its own
    // type argument the bare name cannot supply. Separately — MEASURED, not
    // merely inferred — the synthetic class declares no explicit constructor
    // (this function never has, for any caller), so §12.8 requires its
    // IMPLICIT default constructor's IMPLICIT `base()` chain to resolve: a
    // base whose every constructor takes a parameter fails that check with
    // "the chain must be explicit", even though nothing here ever
    // instantiates anything. Both gaps route through the SAME soundness
    // gate below (a resolveTypes/typecheck error) and decline exactly as any
    // other unresolvable shape does — see parseAndAttachInvariants's header
    // for why this is a silent decline, not a hard reject, and for the
    // measured scope this leaves closed vs. open.
    if (baseSym && !baseSym->name.empty())
        src += " : " + std::string(baseSym->name);
    src += "\n";
    // fix #4514 (§22.12 / §15.1 / §2.8.3 rule 3, v0.306): class-level `const`
    // declarations, so a VERBATIM text this source carries can name one — see
    // buildConstStubs for the measured `[MAX]int` constructor-parameter shape.
    src += buildConstStubs(td);
    // Fields: any field carrying its own fieldInitText gets its real
    // initializer; every other field is a bare type re-declaration (name
    // resolution only — see comment above).
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Field) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " " + mm.name + " : " + metaTypeToSourceText(mm.type);
        if (!mm.fieldInitText.empty()) src += " := " + mm.fieldInitText;
        src += ";\n";
    }
    // fix #3736 (§9.2/§22.8/§2.8.2, v0.286): also re-declare each AUTO-
    // PROPERTY member carrying its own fieldInitText, mirroring the Field
    // loop immediately above — so an auto-property initializer that
    // references a SIBLING auto-property (or static field) of the SAME
    // type (§22.8's "static members of any class" clause, self included)
    // resolves against a live symbol here instead of failing to parse. A
    // computed/block-bodied or static property has no backing-field
    // storage to scaffold (`isAutoProperty` already excludes both — the
    // predicate #2478/#2939 established) and is skipped; an initializer
    // that references one of those still fails to resolve in this
    // throwaway context and correctly falls through to the same
    // reject/silent-fallback boundary the Field loop's own "internal
    // sibling" comment documents.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Property || !mm.isAutoProperty) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " property " + mm.name + " : " + metaTypeToSourceText(mm.type);
        if (!mm.fieldInitText.empty()) src += " := " + mm.fieldInitText;
        src += ";\n";
    }
    // Method stubs, for any self-call the initializer makes.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Method) continue;
        if (mm.isOperator) continue;
        // fix #2842: the field-init synthetic source declares
        // `import <moduleName>;` (#2375), so sibling Named types are visible
        // and the per-stub skip on sibling-type signatures does NOT apply.
        std::string stub = buildMethodStub(mm, td, /*importsModule=*/true);
        if (!stub.empty()) src += stub + "\n";
    }
    // fix #4592: caller-supplied members (the §22.16 `invariant(E);` clauses),
    // spliced after every stub so a clause may name any of them.
    if (!extraMembers.empty()) {
        src += extraMembers;
        if (extraMembers.back() != '\n') src += "\n";
    }
    src += "end " + td.name + ";\n";
    return src;
}

// §2.8.2 / §22.12 / §22.13 (#2351): parse `target.fieldInitText` for a static
// field of an imported generic class, run the three sema passes in a FRESH
// SemaContext, remap the initializer's exprSymbols (field / self-method)
// into mainCtx, and attach the re-parsed ast::FieldDecl (whose `initializer`
// is now a live AST) to the imported FieldSymbol's `decl` slot — mirroring
// ConstructorSymbol::decl / MethodSymbol::decl for the ctor/method-body
// precedents. After this, codegen's genericInstSfinitFn (LlvmBackend_Setup.
// cpp) finds `dynamic_cast<const ast::FieldDecl*>(fs->decl)` non-null and
// lowers+stores the initializer exactly as it already does for a LOCALLY-
// declared generic's own field — no codegen change needed.
//
// §2.8.2's "no restrictions beyond visibility... must reject the export, or
// the import, with a diagnostic naming the shape; it must not alter the
// instantiation's semantics" governs the failure path: unlike the ctor-body
// path (tolerant of benign sema errors) and the method-body path (silently
// declines to attach, falling back to the pre-existing degraded behavior),
// an unattached static field initializer has NO fallback — genericInstSfinitFn
// leaves the shared per-instantiation cell at its zero-init forever, which
// IS an altered instantiation semantics (§2.8.2 clause 2: "an external
// instantiation behaves identically to a same-module instantiation"). So a
// parse/sema failure here is reported as a hard diagnostic into the
// IMPORTING compilation's own DiagnosticEngine (mainCtx.diagnostics()) —
// causing that build to fail cleanly, naming the field and module, rather
// than silently shipping a static field that always reads zero/default.
//
// This is scoped to the reconstruction step itself. It does NOT extend to
// genericInstSfinitFn's own pre-existing, separately-tracked skips
// (collection-literal / inline-array initializers, and a T-DEPENDENT
// initializer whose value type doesn't match the instantiation's field
// type, per #1513's "KNOWN GAP" comment and issue #803) — those remain
// silent zero-inits, an already-recorded limitation this fix does not
// widen or narrow.
//
// Coverage note (#2375, §2.8.2): the synthetic source declares only the
// type being reconstructed, BUT buildSyntheticFieldInitSource also emits
// `import <moduleName>;` and parseAndAttachFieldInit registers the
// exporting module's already-imported PUBLIC sibling types into the fresh
// context — so a PUBLIC sibling-type reference (in the field's declared
// type or in its initializer) now resolves to the real imported
// TypeSymbol and reconstructs. An INTERNAL sibling-type reference still
// fails resolveTypes and rejects via the path below — the CORRECT outcome
// per §2.8.2 (internal types are not in the exported metadata, §13.2, so
// they genuinely do not exist across the module boundary) and the
// #2351 self-pass (a) pin. The ctor-body path's identical "synthetic
// source omits sibling/base types" limitation (#710 / #2290 bug 2) is a
// separate, harder shape (a ctor body can reach arbitrary members, not
// just type names) and remains tracked there.
//
// fix #2874 (§2.8.2 / §22.8 step 2, v0.240): added `bool isStatic` parameter.
// `false` skips the load-time diagnostic on parse/sema failure (a program
// that merely imports the module without ever constructing the offending
// generic still has to build, §2.8.2) and replaces the final `decl = parsedFd`
// attach with `setImportedInstanceFieldInitExpr` — keeping `decl` null so the
// imported-member layout fallbacks continue to consult `importedMemberType`
// (storageType / #662). See `importedInstanceFieldInitExpr` (SemaContext.hpp)
// and the construction-site fallback in `emitOwnFieldInit` for the matching
// reader side.
//
// fix #3736 (§9.2/§22.8/§2.8.2, v0.286): `importedMember` widened from
// `sema::FieldSymbol*` to `sema::Symbol*` so this SAME reconstruction —
// synthetic re-parse, sibling/self remap, type/lambda/pattern-symbol copy —
// also serves a non-static instance AUTO-PROPERTY's initializer (the
// Property twin of the instance-field gap #2874 closed), rather than
// duplicating ~250 lines of remap machinery in a parallel function. Per
// LANE_RULES §6 ("prefer feeding the existing authority"): the field-vs-
// property split is now isolated to exactly the two points that must know
// which AST node shape they are looking at — `target.kind`-branched lookup
// of the reconstructed member below, and the reconstructed initializer
// expression's extraction (`FieldDecl::initializer` vs
// `PropertyDecl::autoInitializer`) — every other step (type resolution,
// symbol remap, lambda params, pattern symbols) is untouched and identical
// for both. `isStatic=true` is a Field-only path by the CALLER's contract
// (ModuleMetadataBuilder.cpp's PropertySymbol branch never populates
// `fieldInitText` for a static property — see that branch's #3736 comment
// for why), so the `isStatic` attach arm below keeps its Field-only cast.
static bool parseAndAttachFieldInit(
    const MetaTypeDecl& td,
    const std::string& moduleName,
    const MetaMember& target,
    sema::Symbol* importedMember,
    const sema::TypeSymbol& importedType,
    sema::SemaContext& mainCtx,
    bool isStatic) {
    std::string source = buildSyntheticFieldInitSource(td, moduleName);

    auto reject = [&](const char* reason) {
        // fix #2874 (§2.8.2, v0.240): the instance-field path MUST NOT become
        // a load-time reject (a program that merely imports the module
        // without ever constructing the offending generic still has to build).
        // The construction-site diagnostic #2860 added to `emitOwnFieldInit`
        // is the fallback that fires at the offending `new`, naming the field
        // and initializer shape — the SAME shape §2.8.2 wants named, just
        // deferred until the construction site that would have observed the
        // wrong value. Silently return so the text remains in
        // `importedInstanceFieldInitText` for that path.
        if (!isStatic) return;
        mainCtx.diagnostics().report(
            Severity::Error, SourceLocation{},
            "cannot import static field '" + td.name + "." + target.name +
                "' from module '" + moduleName +
                "': its initializer could not be reconstructed across the "
                "module boundary (" + reason +
                "); per spec §2.8.2 the import is rejected rather "
                "than silently observing an uninitialized value");
    };

    DiagnosticEngine freshDiag;
    Lexer freshLex(source, freshDiag);
    Parser freshParser(freshLex, freshDiag);
    auto sf = freshParser.parseCompilationUnit();
    if (!sf || freshDiag.hasErrors()) {
        reject("the initializer text failed to re-parse");
        return false;
    }

    // Fresh context/diagnostics must OUTLIVE this function on success — see
    // the ctor/method-body precedents' identical keep-alive discipline.
    auto semaDiag = std::make_shared<DiagnosticEngine>();
    auto freshCtxPtr = std::make_shared<sema::SemaContext>(*semaDiag);
    sema::SemaContext& freshCtx = *freshCtxPtr;
    sema::collectSymbols(*sf, freshCtx);
    if (semaDiag->hasErrors()) {
        reject("symbol collection failed on the reconstructed declaration");
        return false;
    }
    // §2.8.2 / #2375: register the exporting module's PUBLIC sibling types
    // (already imported into mainCtx under `moduleName`) into this fresh
    // context so the `import <moduleName>;` line emitted above resolves a
    // sibling-type reference in the reconstructed initializer / field type
    // to the REAL imported TypeSymbol — no stub declaration and no remap:
    // the reconstructed TypeRefs/exprTypes point directly at the imported
    // symbols, which is exactly what codegen consumes. The reconstructed
    // type itself (td.name) is skipped so its synthetic declaration stays
    // authoritative for any self-reference (preserving pre-#2375 behavior).
    // Internal siblings are absent from mainCtx's imported registry (only
    // publics are exported, §13.2), so they still correctly fail
    // resolveTypes and reject — matching the #2351 self-pass (a) pin.
    if (const std::vector<sema::TypeSymbol*>* siblings =
            mainCtx.importedTypesOf(moduleName)) {
        for (sema::TypeSymbol* sib : *siblings) {
            if (!sib || sib->name.empty()) continue;
            if (sib->name == td.name) continue;
            freshCtx.registerImportedType(moduleName, sib);
            // fix #3748 (§22.8 / §2.8.2 / §18.5, v0.286): a sibling
            // TypeSymbol registered above is the real imported one, so its
            // members are also the real imported symbols. Carry the
            // per-symbol type channels (`importedReturnType` /
            // `importedReturnOwned` / `importedMemberType`) so the
            // re-parsed field initializer sees the same return type for
            // `H.Str()` that the importer's own sema saw. Without this,
            // `H.Str()` (a CallExpr) gets no exprType in freshCtx, the
            // binary `+`'s left/right operand types come out null, the
            // §4.6 typecheck (`"+` requires both operands to be string")
            // rejects at construction time, AND — when the rejection
            // gate is bypassed by passing two non-constant `string`s
            // (`H.Str() + H.Str()`) — the lowered `+` reaches the
            // integer-arithmetic path and emits `add ptr`, an
            // IR-verification abort. Symmetric with the same one-line
            // type-channel carry the ctor/method bodies already do.
            for (sema::Symbol* mem : sib->members) {
                if (!mem) continue;
                if (auto* ms = dynamic_cast<sema::MethodSymbol*>(mem)) {
                    if (sema::TypeRef* rt = mainCtx.importedReturnType(ms))
                        freshCtx.setImportedReturnType(
                            ms, rt, mainCtx.importedReturnOwned(ms));
                } else if (auto* fs = dynamic_cast<sema::FieldSymbol*>(mem)) {
                    if (sema::TypeRef* ty = mainCtx.importedMemberType(fs))
                        freshCtx.setImportedMemberType(fs, ty);
                } else if (auto* ps = dynamic_cast<sema::PropertySymbol*>(
                               mem)) {
                    if (sema::TypeRef* ty = mainCtx.importedMemberType(ps))
                        freshCtx.setImportedMemberType(ps, ty);
                }
                // See markImported's #3748 doc comment: the imported-flag
                // is what the CallExpr typecheck's "imported method" arm
                // keys on (ExprTypecheck_Calls.cpp ~3477). The symbol IS
                // the mainCtx's own imported symbol, so the predicate must
                // be true here too.
                freshCtx.markImported(mem);
            }
        }
    }
    sema::resolveTypes(*sf, freshCtx);
    if (semaDiag->hasErrors()) {
        reject("a referenced type did not resolve inside the module "
               "boundary");
        return false;
    }
    sema::typecheckExpressions(*sf, freshCtx);
    // §14.1/#783 SOUNDNESS GATE, applied here for the same reason: a static
    // field's value is exactly the class of "silently wrong" risk a method's
    // return value is (§2.8.2/#2351 doc comment above) — require a fully
    // clean typecheck of the WHOLE synthetic class (every field's carried
    // initializer, not just `target`'s), since a sibling's ill-typed
    // initializer would leave the module resolvable but ill-formed.
    if (semaDiag->hasErrors()) {
        reject("the initializer references a name not visible across the "
               "module boundary");
        return false;
    }

    sema::TypeSymbol* freshType = nullptr;
    if (freshCtx.module()) {
        for (sema::Symbol* child : freshCtx.module()->children) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(child);
            if (ts && std::string(ts->name) == td.name) { freshType = ts; break; }
        }
    }
    if (!freshType) {
        reject("the reconstructed type could not be located");
        return false;
    }

    // fix #3736: locate the reconstructed member by the SAME kind the
    // exporter tagged it with — a Field target looks for a FieldSymbol via
    // its `->decl` backlink (unchanged, the pre-existing pattern below); a
    // Property target (the only other kind that ever reaches this function
    // — see the widening comment above) is instead located by walking the
    // freshly-PARSED AST (`sf`) directly — `buildSyntheticFieldInitSource`
    // now declares an auto-property member as `property Name : Type :=
    // ...;` (fix #3736) — rather than through a fresh PropertySymbol's own
    // `->decl`. Both would be equally SAFE here (`freshType->members` are
    // never imported symbols — they are fresh, always-non-null-`decl`
    // symbols from this function's own throwaway re-parse — exactly like
    // the pre-existing `freshFs->decl` read four lines below), but the
    // #3152 imported-symbol-seam ratchet (scripts/check_imported_symbol_
    // reads.py) counts `dynamic_cast<const ast::XDecl*>(sym->decl)` sites
    // textually per file regardless of provenance, and this file is
    // already at its baselined ceiling — walking the AST directly avoids
    // adding a new counted site for a pattern this function does not
    // otherwise need (no other step here reads anything off a Property
    // Symbol; the type-level operations below key off `freshType`, and the
    // storage/method remap maps key off `importedType`, the REAL imported
    // type, never `freshType`'s own members).
    const ast::FieldDecl* parsedFd = nullptr;
    const ast::Expr* freshInitExpr = nullptr;
    if (target.kind == MetaMemberKind::Property) {
        const ast::TypeDecl* freshTd = nullptr;
        for (const auto& top : sf->topDecls) {
            auto* cand = dynamic_cast<const ast::TypeDecl*>(top.get());
            if (cand && cand->name == td.name) { freshTd = cand; break; }
        }
        const ast::PropertyDecl* parsedPd = nullptr;
        if (freshTd) {
            for (const auto& mem : freshTd->members) {
                auto* pd = dynamic_cast<const ast::PropertyDecl*>(mem.get());
                if (pd && !pd->isIndexer && pd->name == target.name) {
                    parsedPd = pd;
                    break;
                }
            }
        }
        if (!parsedPd || !parsedPd->autoInitializer) {
            reject("the reconstructed property could not be located, or "
                   "carries no initializer");
            return false;
        }
        freshInitExpr = parsedPd->autoInitializer.get();
    } else {
        sema::FieldSymbol* freshFs = nullptr;
        for (sema::Symbol* m : freshType->members) {
            auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
            if (fs && std::string(fs->name) == target.name) { freshFs = fs; break; }
        }
        if (!freshFs) {
            reject("the reconstructed field could not be located");
            return false;
        }
        parsedFd = dynamic_cast<const ast::FieldDecl*>(freshFs->decl);
        if (!parsedFd || !parsedFd->initializer) {
            reject("the reconstructed declaration carries no initializer");
            return false;
        }
        freshInitExpr = parsedFd->initializer.get();
    }

    // Storage remap map (every FIELD and non-indexer auto-PROPERTY of the
    // class, so a sibling reference in `target`'s initializer binds to the
    // REAL imported symbol — fix #3736 widened this from Field-only to also
    // catch a self auto-property reference, e.g. a §22.8 "static members of
    // any class" initializer referencing a static sibling auto-property of
    // THIS same type).
    std::unordered_map<std::string_view, sema::Symbol*> importedFields;
    for (sema::Symbol* m : importedType.members) {
        bool isStorage = dynamic_cast<sema::FieldSymbol*>(m) != nullptr;
        if (!isStorage) {
            auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
            isStorage = ps && !ps->isIndexer;
        }
        if (isStorage && !m->name.empty()) importedFields[m->name] = m;
    }

    // Self-method remap map (signature-keyed, as in the ctor/method paths).
    std::unordered_map<std::string, sema::Symbol*> importedMethods;
    {
        std::size_t importedIdx = 0;
        for (const MetaMember& mmItem : td.members) {
            if (!metaMemberProducesSymbol(mmItem)) continue;
            if (importedIdx >= importedType.members.size()) break;
            sema::Symbol* sym = importedType.members[importedIdx++];
            if (mmItem.kind == MetaMemberKind::Method && !mmItem.isOperator)
                importedMethods[metaMemberSigKey(mmItem.name, mmItem.params)] =
                    sym;
        }
    }

    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        // fix #3736: widened from FieldSymbol-only to also catch a
        // non-indexer PropertySymbol — a self auto-property reference (or a
        // sibling type's auto-property reference) remaps through the SAME
        // `importedFields` storage map exactly as a field does; see that
        // map's #3736 widening comment above.
        // fix #4514 (§22.12 / §14.1 / §2.8.3 rule 3, v0.306): a class-level
        // `const` named by a re-parsed field initializer. Registered by
        // identity — see the long note on the identical arm in
        // `parseAndAttachCtorBody` for why identity rather than a name remap,
        // and why the stub and this arm are one change. Placed BEFORE the
        // storage arm because the `importedFields` map below holds only
        // FieldSymbols and auto-property storage, so a const falling into it
        // would take the #2375 sibling branch and resolve to nothing.
        if (dynamic_cast<sema::ConstMemberSymbol*>(freshSym)) {
            mainCtx.setExprSymbol(expr, freshSym);
            continue;
        }
        auto* propSym = dynamic_cast<sema::PropertySymbol*>(freshSym);
        if (dynamic_cast<sema::FieldSymbol*>(freshSym) ||
            (propSym && !propSym->isIndexer)) {
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end()) {
                mainCtx.setExprSymbol(expr, it->second);  // self-storage → imported
                continue;
            }
            // #2375: not a self-field/self-property — a SIBLING type's
            // member (e.g. `Box.DefaultSize` in the reconstructed
            // initializer). The sibling was registered as the REAL imported
            // TypeSymbol, so freshSym already IS the imported symbol — copy
            // it as-is. Skipping it (the pre-#2375 behavior for any non-self
            // symbol) left codegen with no symbol for the reference and it
            // silently read zero.
            mainCtx.setExprSymbol(expr, freshSym);
        } else if (auto* fms = dynamic_cast<sema::MethodSymbol*>(freshSym)) {
            auto it = importedMethods.find(freshMethodSigKey(fms));
            if (it != importedMethods.end()) {
                mainCtx.setExprSymbol(expr, it->second);  // self-method → imported
                continue;
            }
            // #2375: a sibling type's method — already the imported symbol.
            // (A self-operator is unreachable here: buildSyntheticFieldInitSource
            // emits no operator stubs, so an operator reference never resolves
            // in freshCtx.)
            mainCtx.setExprSymbol(expr, freshSym);
        } else {
            // No ParameterSymbol/LocalSymbol case: a field initializer is not a
            // body — it has no parameters, and per §22.8/§22.13 a field
            // initializer expression admits no local declarations either.
            // Any other symbol here is a sibling reference (a TypeSymbol or
            // ConstructorSymbol) that already resolves to the real imported
            // symbol — copy it as-is (#2375).
            mainCtx.setExprSymbol(expr, freshSym);
        }
    }

    // Copy resolved-type / expr-type entries so codegen's lowerExpr finds a
    // type for every sub-expression of the initializer (e.g. `new
    // AtomicInteger<int>(0)`'s constructed-type node).
    //
    // fix #2954: and — as in the three sibling paths — a TypeRef naming the
    // class being reconstructed is rebound to the IMPORTED TypeSymbol first,
    // rather than left pointing at this path's synthetic self-class.
    rebindSelfTypeRefsIn(freshCtx, freshType,
                         const_cast<sema::TypeSymbol*>(&importedType));
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        if (astTy && ref) mainCtx.setResolvedType(astTy, ref);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        if (expr && ref) mainCtx.setExprType(expr, ref);
    // fix #2856 (§22.8 Field initializers / §27.3 Lambda expressions, v0.240):
    // carry the initializer's per-lambda parameter lists too — the same table
    // and the same one-line copy #2839 added to the method and ctor paths, and
    // the last of the four reconstruction paths to lack it. §22.8 admits a
    // lambda here verbatim: "Field initializers may reference: constants,
    // literals, static members of any class, `new` expressions, lambdas that
    // capture only the above, primary-constructor parameters (§22.10), and
    // `typeof` expressions."
    //
    // codegen's lowerLambda reads `ctx_.lambdaParams(&lam)` to (a) build the
    // closure function's user parameter type list and (b) bind each incoming
    // argument to its ParameterSymbol in paramSlots_. With no entry it got
    // null, declared the closure with ZERO user parameters, and the value was
    // lost: `public static readonly Seed: int := (lambda(x: int) => x + 1)(6);`
    // in an imported generic class read 0 in the importer and 7 as one module.
    // §2.8.2 clause 2 is the rule broken: "An external instantiation behaves
    // identically to a same-module instantiation of the same generic at the
    // same arguments."
    //
    // Verbatim copy, exactly as for the two type tables above: the keys are the
    // re-parsed ast::LambdaExpr nodes (unique to the fresh AST) and the values
    // are fresh ParameterSymbols with no importer-side twin. The lifetime
    // contract the copy needs is already in place — ownImportedAuxiliary below
    // keeps the fresh context alive. (No ParameterSymbol ARM is needed in the
    // remap loop above, unlike the ctor/method paths: this path's `else` branch
    // already carries every other symbol kind verbatim (#2375), so a lambda
    // parameter's exprSymbol binding was already crossing.)
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        if (lam) mainCtx.setLambdaParams(lam, syms);

    // fix #2857 (§19.3/#1853 / §2.8.2 clause 2, v0.240): and the FIFTH — the
    // initializer's bare-name local-const `case` patterns. See the identical
    // copy in parseAndAttachMethodBody for the full reasoning; verbatim for the
    // same reason as #2818's LocalSymbols (declared inside the re-parsed
    // expression, no importer-side twin), with both kept alive below.
    for (const auto& [pat, sym] : freshCtx.allPatternSymbols())
        if (pat && sym) mainCtx.setPatternSymbol(pat, sym);

    // Attach: importedMember->decl now points at a live ast::FieldDecl whose
    // `initializer` genericInstSfinitFn's existing loop lowers and stores.
    //
    // fix #2874 (§2.8.2 / §22.8 step 2, v0.240): for an INSTANCE field,
    // `decl` MUST stay null — `decl == nullptr` is the discriminator every
    // imported-member fallback in the backend keys on (storageType /
    // recordFieldType / isOwnedReferenceField / …). Instead, publish the
    // parsed initializer into the side table that emitOwnFieldInit /
    // buildRecordValue read ahead of their constant-foldable replay and
    // their construction-site diagnostic fallback. The Expr* rides in `sf`
    // (kept alive via ownImportedAst below), so the side-table pointer is
    // valid for the lifetime of this SemaContext.
    //
    // fix #3736: `isStatic=true` is reached ONLY for a Field target (the
    // writer never populates fieldInitText for a static Property — see
    // ModuleMetadataBuilder.cpp's PropertySymbol branch) — so `parsedFd` is
    // guaranteed non-null on that arm by construction; a Property target
    // (static or not) always takes the `else` arm, keyed generically on
    // `importedMember` (a Symbol* — works identically for a FieldSymbol or
    // a PropertySymbol, since `setImportedInstanceFieldInitExpr` and its
    // `buildRecordValue`/`emitOwnFieldInit` readers are already keyed on
    // the member Symbol*, not on FieldSymbol specifically).
    if (isStatic) {
        importedMember->decl = parsedFd;
    } else if (freshInitExpr) {
        mainCtx.setImportedInstanceFieldInitExpr(importedMember, freshInitExpr);
    }
    mainCtx.ownImportedSource(std::move(source));
    mainCtx.ownImportedAst(std::move(sf));
    mainCtx.ownImportedAuxiliary(semaDiag);
    mainCtx.ownImportedAuxiliary(freshCtxPtr);
    return true;
}

// fix #4592 (§22.16 / §2.8.2, v0.306): re-parse an imported class's OWN
// `invariant(E);` members and publish the resulting expressions on the
// imported TypeSymbol, so `Emitter::emitInvariantCheck` can evaluate them at
// §22.16's construction-exit / public-method-entry / public-method-exit
// points on THIS side of the module boundary.
//
// THE DEFECT THIS CLOSES. `emitInvariantCheck` builds its base-first chain by
// walking `dynamic_cast<const ast::TypeDecl*>(c->decl)` for each class in the
// hierarchy and `continue`s when the cast is null — which is EVERY imported
// class (`TypeSymbol::decl` is never assigned for an import). So a class whose
// base lives in another module had that base's invariants silently dropped
// from the conjunction: not "re-checked elsewhere", never emitted at all.
// MEASURED across a real `--import` boundary, `--checked-contracts` on both
// halves, a base with `invariant(v > 0);` constructed with `v = -1`:
//   pre-fix   exit 42 — construction completed, no ContractViolationException
//   post-fix  exit 99 — the catch arm caught the violation
// The byte-identical same-module program answers 99 on both, which is exactly
// why the gap was invisible.
//
// WHY A RE-PARSE AND NOT AN ACCESSOR SWAP. Unlike the sibling imported-symbol
// gaps whose symbol side already holds the answer, the fact this needs is an
// `ast::Expr` to lower. `TypeSymbol::invariantContracts` carries only
// §2.8.2 `canonicalText`, which is an identity artifact (measured: `v > 0` on
// module `InvLibA`'s class `BaseA` records as `InvLibA.BaseA.v > 0`). The
// exporter therefore now also carries the clause's verbatim source text
// (`MetaTypeDecl::ownInvariantText`, the §2.8.2 display-text channel), and
// this function turns it back into live, symbol-bound expressions by exactly
// the reconstruction discipline the ctor / dtor / method / property /
// field-init paths above already use: parse a synthetic re-declaration of the
// class, run the three sema passes in a FRESH context, remap every expression
// symbol onto the REAL imported member, copy the four AST-keyed side tables,
// and keep the fresh source/AST/context alive for the compilation's lifetime.
//
// FAILURE POLICY — RECORDED CHOICE (CLAUDE.md §4). A reconstruction that does
// not type-check silently declines to publish, leaving the pre-fix behaviour
// (the clause is not evaluated in the importer). This matches
// `parseAndAttachMethodBody`'s policy rather than `parseAndAttachFieldInit`'s
// hard reject, and the reason is the direction of the harm: a hard reject
// would turn programs that build today into programs that do not build, to
// buy a check that is off by default (§22.16 makes clause evaluation a
// checked-contracts-mode behaviour, and the loader cannot see the mode).
//
// fix #4599 residual 1 (§22.16 / §11.1 / §2.8.2, v0.306): a clause naming an
// INHERITED member now ALSO reconstructs, for a NON-GENERIC immediate base
// THAT HAS AN ACCESSIBLE PARAMETERLESS CONSTRUCTOR (see "NOT covered" below
// for why that second condition exists — it was not part of the original
// filing and was found only by driving a real build). The synthetic source
// re-declares only the class's OWN members — the #710/#2290 limitation
// every OTHER reconstruction path in this file shares, because those paths
// need the base's METHOD BODIES / CTOR ARGUMENTS (executable code, requiring
// a full textual re-declaration of the base chain to re-parse against) —
// but an invariant clause only needs to LOOK UP the base's storage, not
// execute anything over it. `importedType.baseTypes` (populated by pass 2's
// `reconstructTypeSymbol`, itself already resolved — codegen's
// `baseClassOf` reads the same field) already gives a REAL, already-fully-
// loaded base `TypeSymbol*`, whichever module it declares in and however
// many ancestors above THAT are already resolved on IT. So rather than
// re-declaring the base's storage as a textual stub (#710's approach,
// needed only when the reconstruction must execute code), this function
// instead points the synthetic `Derived`'s base clause AT the real symbol
// (`registerForFreshCtx` below) — member lookup then walks the REAL,
// pre-existing base chain, and an inherited field's `exprSymbol` already IS
// the real `FieldSymbol`, needing no remap (see the loop below's "#2375
// sibling member" arm, which this shape also falls into, unmodified).
//
// NOT covered, and both routed through the SAME resolveTypes/typecheck
// soundness gate below (a silent decline, same policy as any other
// unresolvable shape):
//
//   * a GENERIC base (`buildSyntheticFieldInitSource`'s ` : Base` emits a
//     bare name — see its own comment) — resolveTypes reports the arity
//     mismatch it cannot supply;
//   * a base whose EVERY constructor takes a parameter. MEASURED, not
//     inferred: this function's synthetic class declares no constructor of
//     its own (it never has, for any caller of `buildSyntheticFieldInitSource`),
//     so §12.8 requires its IMPLICIT default constructor's IMPLICIT `base()`
//     chain to resolve — "implicit `base()` chain: base class `X` has no
//     parameterless constructor; the chain must be explicit (§12.8)" — even
//     though the synthetic module never instantiates anything. Closing THIS
//     narrower gap needs a forwarding constructor whose parameter list
//     type-matches one of the base's, which for anything beyond primitive
//     parameter types needs those types' own declarations reconstructed in
//     the fresh context too — the SAME cross-type textual-reconstruction
//     surface #710/#2290 already tracks, reached through a different door
//     (the implicit-chain check) than #710's filed repro (an explicit
//     cross-type call in a ctor body). Filed as #710's narrower residual
//     rather than solved here, per CLAUDE.md §4 (decline-with-reasons, not
//     a silent partial fix): supplying literal placeholder argument values
//     would be unsound name-shopping (which literal is "right" for an
//     arbitrary parameter type is not this function's decision to make),
//     and a generalized "reconstruct the base's declaration too" is
//     #710/#2290's stated direction, not a narrow addition here.
//
// Verified to reconstruct, by two-module probe: a clause over the class's own
// `private` field (those cross for layout, #2456), a clause naming a
// class-level `const` (#4514's stubs), a clause on a GENERIC imported class
// instantiated in the importer, and (residual 1's own probe) a clause on a
// class naming a `public` field declared on its NON-GENERIC, parameterless-
// constructor immediate base, the base declared in the SAME module and —
// separately — in a DIFFERENT, transitively-imported module. Also verified,
// by the SAME probe technique, that a base whose only constructor takes an
// argument reproduces the exact §12.8 diagnostic above and the reconstruction
// declines (pre-fix behaviour preserved, not a regression).
static bool parseAndAttachInvariants(const MetaTypeDecl& td,
                                     const std::string& moduleName,
                                     sema::TypeSymbol& importedType,
                                     sema::SemaContext& mainCtx) {
    if (td.ownInvariantText.empty()) return false;

    // fix #4599 residual 1: the first CLASS entry of `importedType.baseTypes`
    // — mirrors `Emitter::baseClassOf`'s exact filter (LlvmBackend_Setup.cpp)
    // so this function and codegen's own base-chain walk agree on which base
    // is "the" class base when an interface base sits ahead of it in the
    // list (`reconstructTypeSymbol` pushes every resolved Named base,
    // interfaces included, in `td.baseTypes` order).
    sema::TypeSymbol* baseSym = nullptr;
    for (sema::TypeSymbol* b : importedType.baseTypes) {
        if (b && b->kind == ast::TypeDeclKind::Class) { baseSym = b; break; }
    }

    std::string source = buildSyntheticFieldInitSource(
        td, moduleName, td.ownInvariantText, baseSym);

    DiagnosticEngine freshDiag;
    Lexer freshLex(source, freshDiag);
    Parser freshParser(freshLex, freshDiag);
    auto sf = freshParser.parseCompilationUnit();
    if (!sf || freshDiag.hasErrors()) return false;

    // The fresh context and its diagnostics must OUTLIVE this function: the
    // published Expr* nodes live in `sf` and their symbols/types live in the
    // fresh context's arena. Engine first (destroyed last), context second —
    // the same ordering discipline every sibling path documents.
    auto semaDiag = std::make_shared<DiagnosticEngine>();
    auto freshCtxPtr = std::make_shared<sema::SemaContext>(*semaDiag);
    sema::SemaContext& freshCtx = *freshCtxPtr;
    sema::collectSymbols(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    // §2.8.2 / #2375: make the exporting module's PUBLIC sibling types visible
    // to the `import <moduleName>;` line the synthetic source emits, so a
    // clause naming one resolves to the REAL imported symbol. Same registration
    // (and same #3748 per-symbol type-channel carry) as the field-init path.
    //
    // fix #4599 residual 1: register the REAL base symbol the same way, under
    // ITS OWN declaring module (which may differ from `moduleName` — see
    // `buildSyntheticFieldInitSource`'s matching `import` line) — so the
    // ` : Base` clause in the synthetic header binds to the real, already
    // fully-resolved symbol rather than failing to resolve at all.
    auto registerForFreshCtx = [&](sema::TypeSymbol* sym) {
        if (!sym || sym->name.empty()) return;
        freshCtx.registerImportedType(sym->moduleName, sym);
        for (sema::Symbol* mem : sym->members) {
            if (!mem) continue;
            if (auto* ms = dynamic_cast<sema::MethodSymbol*>(mem)) {
                if (sema::TypeRef* rt = mainCtx.importedReturnType(ms))
                    freshCtx.setImportedReturnType(
                        ms, rt, mainCtx.importedReturnOwned(ms));
            } else if (auto* fs = dynamic_cast<sema::FieldSymbol*>(mem)) {
                if (sema::TypeRef* ty = mainCtx.importedMemberType(fs))
                    freshCtx.setImportedMemberType(fs, ty);
            } else if (auto* ps = dynamic_cast<sema::PropertySymbol*>(mem)) {
                if (sema::TypeRef* ty = mainCtx.importedMemberType(ps))
                    freshCtx.setImportedMemberType(ps, ty);
            }
            freshCtx.markImported(mem);
        }
    };
    if (baseSym) registerForFreshCtx(baseSym);
    if (const std::vector<sema::TypeSymbol*>* siblings =
            mainCtx.importedTypesOf(moduleName)) {
        for (sema::TypeSymbol* sib : *siblings) {
            if (!sib || sib->name.empty()) continue;
            if (sib->name == td.name) continue;
            registerForFreshCtx(sib);
        }
    }
    sema::resolveTypes(*sf, freshCtx);
    if (semaDiag->hasErrors()) return false;
    sema::typecheckExpressions(*sf, freshCtx);
    // §14.1/#783 soundness gate: a clause that does not type-check cleanly
    // would lower to a silently-wrong predicate (or to nothing), which is the
    // very failure mode this fix exists to remove — decline instead.
    if (semaDiag->hasErrors()) return false;

    sema::TypeSymbol* freshType = nullptr;
    if (freshCtx.module()) {
        for (sema::Symbol* child : freshCtx.module()->children) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(child);
            if (ts && std::string(ts->name) == td.name) { freshType = ts; break; }
        }
    }
    if (!freshType) return false;

    // Locate the re-parsed `invariant` members by walking the freshly-PARSED
    // AST directly. `InvariantDecl` is not modelled as a `Symbol` at all
    // (SymbolCollection creates none — see TypeResolution_Checks.cpp's #4115
    // note), so the AST is the only route; walking `sf` also keeps this
    // function off the #3152 seam ratchet's `<sym>->decl` cast count, for the
    // reason parseAndAttachFieldInit's #3736 Property arm records.
    const ast::TypeDecl* freshTd = nullptr;
    for (const auto& top : sf->topDecls) {
        auto* cand = dynamic_cast<const ast::TypeDecl*>(top.get());
        if (cand && cand->name == td.name) { freshTd = cand; break; }
    }
    if (!freshTd) return false;
    std::vector<const ast::Expr*> exprs;
    for (const auto& member : freshTd->members) {
        auto* inv = dynamic_cast<const ast::InvariantDecl*>(member.get());
        if (!inv || !inv->expr || inv->isStatic) continue;
        exprs.push_back(inv->expr.get());
    }
    if (exprs.empty()) return false;

    // Storage remap: every FIELD and non-indexer PROPERTY of the imported
    // class, so a clause's bare `v` binds to the REAL imported symbol —
    // `fieldIndex_` / `storageMemberOf` are keyed on it, and a fresh symbol
    // would make codegen build a separate struct identity for the same
    // storage (the #3207 measurement).
    std::unordered_map<std::string_view, sema::Symbol*> importedFields;
    for (sema::Symbol* m : importedType.members) {
        bool isStorage = dynamic_cast<sema::FieldSymbol*>(m) != nullptr;
        if (!isStorage) {
            auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
            isStorage = ps && !ps->isIndexer;
        }
        if (isStorage && !m->name.empty()) importedFields[m->name] = m;
    }
    // Self-method remap, signature-keyed — §22.16 admits a clause calling a
    // `pure` public method of the same instance.
    std::unordered_map<std::string, sema::Symbol*> importedMethods;
    {
        std::size_t importedIdx = 0;
        for (const MetaMember& mmItem : td.members) {
            if (!metaMemberProducesSymbol(mmItem)) continue;
            if (importedIdx >= importedType.members.size()) break;
            sema::Symbol* sym = importedType.members[importedIdx++];
            if (mmItem.kind == MetaMemberKind::Method && !mmItem.isOperator)
                importedMethods[metaMemberSigKey(mmItem.name, mmItem.params)] =
                    sym;
        }
    }

    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        // fix #4514's arm, for the same reason: a class-level `const` named by
        // a clause is registered BY IDENTITY (it has no storage and no runtime
        // identity to unify) and must not fall into the storage map below.
        if (dynamic_cast<sema::ConstMemberSymbol*>(freshSym)) {
            mainCtx.setExprSymbol(expr, freshSym);
            continue;
        }
        auto* propSym = dynamic_cast<sema::PropertySymbol*>(freshSym);
        if (dynamic_cast<sema::FieldSymbol*>(freshSym) ||
            (propSym && !propSym->isIndexer)) {
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end()) {
                mainCtx.setExprSymbol(expr, it->second);
                continue;
            }
            mainCtx.setExprSymbol(expr, freshSym);  // #2375 sibling member
        } else if (auto* fms = dynamic_cast<sema::MethodSymbol*>(freshSym)) {
            auto it = importedMethods.find(freshMethodSigKey(fms));
            if (it != importedMethods.end()) {
                mainCtx.setExprSymbol(expr, it->second);
                continue;
            }
            mainCtx.setExprSymbol(expr, freshSym);  // #2375 sibling method
        } else {
            mainCtx.setExprSymbol(expr, freshSym);
        }
    }

    // The four AST-keyed side tables, copied exactly as the sibling paths do —
    // a clause sub-expression with no `exprType` lowers as a null and the
    // predicate silently answers the wrong thing (#3748's measurement on the
    // field-init path). Self-TypeRefs are rebound to the imported symbol first
    // (#2954), so a `typeof`/`is` over the class itself does not name the
    // synthetic shadow.
    rebindSelfTypeRefsIn(freshCtx, freshType, &importedType);
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        if (astTy && ref) mainCtx.setResolvedType(astTy, ref);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        if (expr && ref) mainCtx.setExprType(expr, ref);
    for (const auto& [lam, syms] : freshCtx.allLambdaParams())
        if (lam) mainCtx.setLambdaParams(lam, syms);
    for (const auto& [pat, sym] : freshCtx.allPatternSymbols())
        if (pat && sym) mainCtx.setPatternSymbol(pat, sym);

    importedType.importedInvariantExprs = std::move(exprs);
    mainCtx.ownImportedSource(std::move(source));
    mainCtx.ownImportedAst(std::move(sf));
    mainCtx.ownImportedAuxiliary(semaDiag);
    mainCtx.ownImportedAuxiliary(freshCtxPtr);
    return true;
}

// fix #4599 residual 2 (§22.16 / §11.6 / §2.8.2, v0.306): recurse into a
// §11.6 NESTED imported class's OWN `invariant(E);` clauses. `convertTypeDecl`
// populates `ownInvariantText` uniformly for every class it visits — nested
// included, since it rides each member's `nestedType` block — but the pass-3b
// caller below only walked `md.types`, i.e. TOP-LEVEL types, so a nested
// class's clauses rode the wire and were never re-parsed: `emitInvariantCheck`
// silently skipped them, the pre-#4592 behaviour for that shape.
//
// Sync-walks `td.members` against `t.members` using `metaMemberProducesSymbol`
// — the same discipline `parseAndAttachInvariants`'s own importedMethods remap
// above uses — rather than a `localTypes` qualified-name lookup: pass 2's
// `reconstructTypeSymbol` (called recursively for every nested type, see its
// `MetaMemberKind::NestedType` arm) has already populated each
// `NestedTypeSymbol::type` by the time pass 3b runs, so the sync-walk finds
// the fully-reconstructed inner TypeSymbol directly, with no second lookup
// that could resolve a different type on a same-simple-name collision across
// sibling parents.
//
// Container kind is NOT gated here (unlike the Class-only check below): §11.6
// permits a nested type inside a class OR a record body, and this function's
// job is only to find the nested class — the Class-kind gate that decides
// whether §22.16 applies at all is applied per nested type, exactly as the
// top-level loop applies it per top-level type. Recurses so a doubly-nested
// class's clauses are reached too.
static void parseAndAttachInvariantsNested(const MetaTypeDecl& td,
                                           const std::string& moduleName,
                                           sema::TypeSymbol& t,
                                           sema::SemaContext& ctx) {
    std::size_t idx = 0;
    for (const MetaMember& mm : td.members) {
        if (!metaMemberProducesSymbol(mm)) continue;
        if (idx >= t.members.size()) break;
        sema::Symbol* sym = t.members[idx++];
        if (mm.kind != MetaMemberKind::NestedType || !mm.nestedType) continue;
        auto* nts = dynamic_cast<sema::NestedTypeSymbol*>(sym);
        if (!nts || !nts->type) continue;
        const MetaTypeDecl& ntd = *mm.nestedType;
        if (nts->type->kind == ast::TypeDeclKind::Class &&
            !ntd.ownInvariantText.empty()) {
            parseAndAttachInvariants(ntd, moduleName, *nts->type, ctx);
        }
        parseAndAttachInvariantsNested(ntd, moduleName, *nts->type, ctx);
    }
}

}  // namespace

void stripContractFacts(MetaMember& member) {
    member.isPure = false;
    member.contracts.clear();
    member.modifiers.erase(
        std::remove(member.modifiers.begin(), member.modifiers.end(), "pure"),
        member.modifiers.end());
    if (member.nestedType) {
        for (MetaMember& nested : member.nestedType->members)
            stripContractFacts(nested);
        member.nestedType->invariants.clear();
        // fix #4592 (§22.16 / §2.8.3 rule 3, v0.306): the invariant SOURCE
        // text is a contract fact of exactly the kind this walk exists to
        // drop. See the sibling clear below for why leaving it would make an
        // opaque pre-carriage artifact disagree with a carrying one.
        member.nestedType->ownInvariantText.clear();
    }
}

void stripContractFacts(MetaTypeDecl& type) {
    type.invariants.clear();
    // fix #4592 (§22.16 / §2.8.3 rule 3, v0.306): `ownInvariantText` rides
    // beside `invariants` and must be dropped with them. `metadataDescriptions
    // Agree` uses this walk when EITHER duplicate description is opaque (a
    // pre-carriage artifact "makes no behavioral claim"); clearing the records
    // but keeping the text would leave `operator==` comparing "" against
    // `invariant(v > 0);` and report a §2.8.3 rule 3 staleness disagreement
    // for two descriptions the rule says agree.
    type.ownInvariantText.clear();
    for (MetaMember& member : type.members) stripContractFacts(member);
}

void stripContractFacts(MetaFunction& function) {
    function.isPure = false;
    function.contracts.clear();
    function.modifiers.erase(
        std::remove(function.modifiers.begin(), function.modifiers.end(), "pure"),
        function.modifiers.end());
}

// §2.8.3 rule 3 / §2.8.2 (#2054): a pre-carriage artifact makes no behavioral
// claim. When either duplicate description is opaque, compare the structural
// interface only; when both carry records, every behavioral field must agree.
bool metadataDescriptionsAgree(const ModuleMetadata& lhs,
                               const ModuleMetadata& rhs) {
    if (lhs.moduleName != rhs.moduleName) return false;
    if (lhs.contractRecordsCarried && rhs.contractRecordsCarried)
        return lhs == rhs;
    ModuleMetadata a = lhs;
    ModuleMetadata b = rhs;
    for (MetaTypeDecl& type : a.types) stripContractFacts(type);
    for (MetaTypeDecl& type : b.types) stripContractFacts(type);
    for (MetaFunction& function : a.functions) stripContractFacts(function);
    for (MetaFunction& function : b.functions) stripContractFacts(function);
    a.contractRecordsCarried = true;
    b.contractRecordsCarried = true;
    return a == b;
}

// fix #3581 (§11.6 / §2.8.3 rule 3 / §2.8.2, v0.286): register a §11.6
// NESTED type's shell in BOTH the per-loader `localTypes` map AND the
// SemaContext-wide cross-module `importedByModule_` table, recursively
// (because a nested type can itself contain nested types). Pre-#3581 only
// the module's TOP-LEVEL types were registered in `importedByModule_` and
// nested shells were created only later in pass 2's `reconstructTypeSymbol`
// — too late for a cross-module consumer whose pass 2 walks first (the
// issue's three test rows). Pass 1 pre-registers the shell, and pass 2's
// `reconstructTypeSymbol` mutates it in place so the TypeRef's declaration
// pointer is preserved across the shell→full upgrade.
static void registerNestedTypeShells(
    const MetaTypeDecl& td, sema::SemaContext& ctx, sema::TypeSymbol& parent,
    std::unordered_map<std::string, sema::TypeSymbol*>& localTypes) {
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::NestedType || !mm.nestedType) continue;
        const MetaTypeDecl& ntd = *mm.nestedType;
        auto* t = ctx.makeImportedSymbol<sema::TypeSymbol>();
        t->name = ctx.internString(ntd.name);
        t->kind = fromMetaTypeDeclKind(ntd.kind);
        t->visibility = fromMetaVisibility(ntd.visibility);
        t->modifiers.clear();
        for (const std::string& m : ntd.modifiers)
            t->modifiers.push_back(ctx.internString(m));
        t->scope = parent.memberScope;
        t->memberScope = ctx.newScope();
        // §9.2 (schema v4, issue #501): positional-record flag.
        t->hasPositionalFields = ntd.hasPositionalFields;
        // #1488 / §2.8.3 rule 3: a nested type declares in the same module
        // as its enclosing type (§11.6 — nesting is not a separate module).
        t->moduleName = parent.moduleName;
        // fix #3529 (§11.6 / §2.8.3 rule 3, v0.283): enclosing-type path.
        t->enclosingTypePath = parent.qualifiedTypeName();
        localTypes[t->qualifiedTypeName()] = t;
        ctx.registerImportedType(parent.moduleName, t);
        // Recurse: a §11.6 type may itself contain nested types.
        registerNestedTypeShells(ntd, ctx, *t, localTypes);
    }
}

// §2.5 / #1487 — Pass 1: create a TypeSymbol shell per exported type AND
// register it globally, so a cross-reference (a member/base whose type is a
// sibling or a type from ANOTHER dependency) resolves by name in pass 2
// regardless of the order in which dependencies are loaded. `localTypes` records
// this module's own name→shell map (same-module priority in Loader::lookupNamed);
// `created` is parallel to md.types for pass 2. Registration is done HERE (it
// used to happen at the end of pass 2, per type) so that a Named reference from a
// dependency loaded *before* this one still binds to this type's shell — the
// order-dependent silent-degradation fixed by #1487.
static void loadMetadataPass1(
    const ModuleMetadata& md, sema::SemaContext& ctx,
    std::unordered_map<std::string, sema::TypeSymbol*>& localTypes,
    std::vector<sema::TypeSymbol*>& created) {
    created.reserve(md.types.size());
    for (const MetaTypeDecl& td : md.types) {
        auto* t = ctx.makeImportedSymbol<sema::TypeSymbol>();
        t->name = ctx.internString(td.name);
        t->kind = fromMetaTypeDeclKind(td.kind);
        t->visibility = fromMetaVisibility(td.visibility);
        t->modifiers.clear();
        for (const std::string& m : td.modifiers)
            t->modifiers.push_back(ctx.internString(m));
        t->scope = sema::kModuleScope;
        t->memberScope = ctx.newScope();
        // §9.2 (schema v4, issue #501): carry the positional-record flag so the
        // §9.2 auto-equality check in ExprTypecheck_Exprs.cpp reads it from
        // TypeSymbol::hasPositionalFields (no ast::TypeDecl exists for imported types).
        t->hasPositionalFields = td.hasPositionalFields;
        // fix #2950 (§12 / §31.4 / §2.8.3 rule 4, v0.240): the built-in
        // exception base, i.e. whether this class carries the §31.4 header. It
        // is set in PASS 1 (not pass 2, where the other base-type work happens)
        // because it decides the class's LAYOUT and is a property of the class
        // itself, not of any resolvable base symbol — nothing about it needs
        // the other dependencies' shells to exist yet.
        t->builtinExceptionBase = td.builtinExceptionBase;
        // fix #3191 (§3.6 / §28.9 / §2.8.2, family M2 of #3152): the §3.6
        // trivial-dispose verdict, replayed here for the same reason as the
        // exception base above — it is a property of the class itself, computed
        // by the exporter (which can see the destructor), and needs no other
        // dependency's shell to exist yet. See the sibling site's comment.
        //
        // fix #3179 (§9.8 tier 2+ / §2.8.3 rule 4, family F10 of umbrella
        // #3152) NOTE: the §9.8 class wire SHAPE and `polymorphic(...)`
        // REGISTRY fields on the symbol are NOT populated here in pass 1.
        // They will be populated in pass 2 along with the codegen-ABI half
        // (#3162 / #3191 family — the synthesized `Deserialize` construction
        // for an IMPORTED class). Populating them in pass 1 alone, on this
        // lane, would expose the §9.8 class surface to consumers without the
        // construction machinery they need, turning the imported-class
        // `b.Serialize(s)` call into a silent wrong-value path
        // (#3140's T9/T10 over-acceptance controls pin the pre-#3179
        // "still rejected" reading; #3179 itself records that those flip
        // when the ABI lands, not when the wire alone lands). The wire
        // carriage is the deliverable this lane lands; the symbol-side
        // replay joins it when #3162/#3191 follow-up land.
        t->importedNonTrivialDispose = td.nonTrivialDispose;
        t->invariantContracts = toContractRecords(td.invariants);
        // #1488 / §2.8.3 rule 3: this type's declaring-module identity, as
        // reconstructed from the dependency's own metadata — the far side of
        // the pairing with SymbolCollection::collectTypeDecl's local
        // population. Both sides read `md.moduleName`'s equivalent (the local
        // side via the ModuleDecl, this side directly from the artifact), so
        // an importer's external symbol declarations and the exporter's
        // definitions always agree.
        t->moduleName = md.moduleName;
        localTypes[td.name] = t;
        created.push_back(t);
        ctx.registerImportedType(md.moduleName, t);
        // fix #3581: register this type's §11.6 NESTED-type shells in both
        // `localTypes` and the SemaContext's cross-module `importedByModule_`
        // table so cross-module consumers (TMidC's pass 2 looking up
        // TBase.Outer.Box when the deps were listed `--import tmidc.o
        // --import tbase.o`, putting TMidC's pass 2 BEFORE TBase's pass 2)
        // can resolve nested types by qualified name from pass 1 onwards.
        // Pass 2's `reconstructTypeSymbol` mutates those shells in place so
        // the TypeRef's `declaration` pointer, which was bound to the shell
        // at convert-time, points to the populated symbol by codegen time.
        registerNestedTypeShells(td, ctx, *t, localTypes);
    }
}

// §2.5 / #1487 — Pass 2 (+ pass 3 + side-table publish): fill generic params,
// base types, and members (every dependency's shells are already registered by
// pass 1, so a Named ref resolves by name whatever the load order), then re-parse
// exported generic bodies and publish the loader's member-type side tables.
static void loadMetadataPass2(
    const ModuleMetadata& md, sema::SemaContext& ctx,
    std::unordered_map<std::string, sema::TypeSymbol*>& localTypes,
    const std::vector<sema::TypeSymbol*>& created) {
    Loader loader{ctx, localTypes};

    for (std::size_t i = 0; i < md.types.size(); ++i) {
        const MetaTypeDecl& td = md.types[i];
        sema::TypeSymbol* t = created[i];
        t->genericParams =
            loader.convertGenericParams(td.genericParams, t->memberScope);
        // fix #3151 (§2.8.3 rule 4, v0.252): mirror the local rule from
        // reconstructTypeSymbol above — push to `baseTypeRefs` and
        // `baseTypes` under ONE guard (`kind == Named && declaration`) so
        // the two vectors stay index-aligned. See the sibling fix above
        // for the design note (CLAUDE.md §4 — recorded decision).
        for (const MetaType& base : td.baseTypes) {
            sema::TypeRef* ref = loader.convert(base);
            if (ref->kind == sema::TypeRef::Kind::Named && ref->declaration) {
                t->baseTypeRefs.push_back(ref);
                t->baseTypes.push_back(ref->declaration);
            }
        }
        // fix #3139 — see the sibling restore in `reconstructTypeSymbol`.
        for (const MetaType& iface : td.builtinInterfaceBases)
            t->nonSymbolBaseRefs.push_back(loader.convert(iface));
        for (const MetaMember& mm : td.members) {
            if (sema::Symbol* m = loader.convertMember(mm, *t))
                t->members.push_back(m);
        }
        // fix #3144 — see the sibling call in `reconstructTypeSymbol`. Pass 2,
        // not pass 1: a payload's type may name a sibling or another
        // dependency's type, so every shell must already be registered.
        loader.convertChoiceCases(td, *t);
        // (registration moved to pass 1 for order-independent by-name binding.)
    }

    // fix #3540 (§11.6 / §2.8.3 rule 3, v0.285): every type of this module is
    // now registered in `localTypes` — the top-level ones by pass 1, the §11.6
    // nested ones by the member walk just above — so retry the Named references
    // that could not bind while that walk was still in progress. Placed before
    // pass 3, whose generic-body re-parse reads the member types this fills in.
    // See `Loader::resolvePendingNamed`.
    loader.resolvePendingNamed();

    // fix #3748 (§22.8 / §2.8.2, v0.286): publish the per-method
    // `importedReturnType` and per-member `importedMemberType` side
    // tables BEFORE pass 3's `parseAndAttachFieldInit` runs. The
    // load-time field-init reconstruction builds a fresh SemaContext
    // and re-parses the initializer expression; that synthetic AST
    // references the IMPORTED sibling methods (e.g. `H.Str()`), and
    // the typecheck arm that gives the CallExpr its exprType
    // (ExprTypecheck_Calls.cpp ~3477) reads `ctx.importedReturnType`
    // on the fresh context. Without this early publish, the
    // reconstruction silently leaves the call's exprType null, the
    // §4.6 string `+` check passes only because both operands are
    // equally untyped, and the codegen path then routes the binary
    // `+` through the integer-arithmetic fallthrough that emits
    // `add ptr` (an IR-verification abort). Published here for the
    // same reason `resolvePendingNamed` is placed here — pass 3's body
    // re-parse is the first consumer that actually reads these tables.
    //
    // fix #3828 (§26.1 / §26.5, v0.287): this is the EARLIER of TWO
    // publishes, not a move. #3748 originally deleted the end-of-
    // function one, which silently dropped everything pass 4 adds —
    // every imported EXTENSION method's return type and extension
    // property's type. Both must run; see the twin block at the end of
    // this function for why replaying is safe.
    for (auto& [m, ret] : loader.returnTypes_)
        ctx.setImportedReturnType(m, ret, loader.returnOwned_[m]);
    for (auto& [m, ty] : loader.fieldTypes_)
        ctx.setImportedMemberType(m, ty);

    // Pass 3 (§14/#662c, schema v5): for imported generic CLASSES, re-parse
    // exported ctor/dtor body text and attach ast::ConstructorDecl /
    // ast::DestructorDecl to the reconstructed symbols so inlineCtorBody can
    // emit the body with the per-instantiation genericSubst_ active.
    for (std::size_t i = 0; i < md.types.size(); ++i) {
        const MetaTypeDecl& td = md.types[i];
        sema::TypeSymbol* t = created[i];
        // Only generic classes and records carry exported constructor bodies.
        // fix #3645 (§9.7 / §28.9 / §2.8.3 rule 4, v0.286): widen the gate to
        // also admit a NON-GENERIC class whose members carry a non-empty
        // `methodBodyText` entry — the §14.1/#783 generic-only gate would
        // otherwise skip them, and the body-walk proof
        // `isProvablyFreshRecordFactoryMethod` needs `ms->decl` populated to
        // admit the imported fresh-construction factory shape
        // (`Make(n: int): Pt => new Pt(...)`). The widening is conditioned on
        // a present methodBodyText so the §14.1/#1937 soundness gate on
        // generic-method bodies stays in force for the GENERIC case (which
        // still goes through the generic-method attach path immediately
        // below).
        bool hasMethodBodyText = false;
        if (t->kind == ast::TypeDeclKind::Class && td.genericParams.empty()) {
            for (const MetaMember& mm : td.members) {
                if (mm.kind == MetaMemberKind::Method &&
                    !mm.methodBodyText.empty()) {
                    hasMethodBodyText = true;
                    break;
                }
            }
        }
        if ((t->kind != ast::TypeDeclKind::Class &&
             t->kind != ast::TypeDeclKind::Record) ||
            (t->kind == ast::TypeDeclKind::Class && td.genericParams.empty() &&
             !hasMethodBodyText))
            continue;

        // Ctor bodies: find each Constructor member with non-empty ctorBodyText.
        for (const MetaMember& mm : td.members) {
            if (mm.kind != MetaMemberKind::Constructor || mm.ctorBodyText.empty())
                continue;
            // Locate the matching ConstructorSymbol in t->members.
            // §14/#705 (facet b): match by full parameter-type signature, not
            // just arity.  Two ctors may share param count but differ in types
            // (§22.7 ctor overloads); arity-only matching attaches the wrong
            // body.  We compare each param pair using metaTypeSigText (MetaParam
            // side) and typeRefSigText (imported ParameterSymbol side); both
            // produce the same canonical text for the same underlying type.
            sema::ConstructorSymbol* importedCs = nullptr;
            {
                const std::string mmSig = metaMemberSigKey("", mm.params);
                for (sema::Symbol* m : t->members) {
                    auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
                    if (!cs || cs->isStatic) continue;
                    if (cs->params.size() != mm.params.size()) continue;
                    // Build sig key from imported ctor params.
                    std::string csSig;
                    csSig += '(';
                    for (std::size_t pi = 0; pi < cs->params.size(); ++pi) {
                        if (pi) csSig += ',';
                        const auto* ps = dynamic_cast<const sema::ParameterSymbol*>(
                            cs->params[pi]);
                        csSig += typeRefSigText(ps ? ps->resolvedDeclType : nullptr);
                    }
                    csSig += ')';
                    if (csSig == mmSig) {
                        importedCs = cs;
                        break;
                    }
                }
            }
            if (importedCs) {
                parseAndAttachCtorBody(td, mm.ctorBodyText, importedCs, *t, ctx);
            }
        }

        // §14.1 / #783: method bodies — find each Method member with a non-empty
        // methodBodyText and attach the re-parsed ast::MethodDecl to the matching
        // imported MethodSymbol, so codegen can monomorphize it per instantiation
        // (the cross-module complement of the local generic-method path).
        //
        // fix #3226 (§25 / §14.1, v0.254): operators are now ALSO exported with
        // methodBodyText (ModuleMetadataBuilder.cpp OperatorDecl arm), so the
        // matching loop must include them. The previous `ms->isOperator` skip
        // excluded operators; without this change every imported generic-record
        // operator's body stays unattached and codegen's HARD route at
        // emitMethodCall line 10449 then refuses to inline-specialize the call
        // (it required a MethodDecl on the imported symbol), falling through to
        // the i32-PLACEHOLDER template and IR-verify failing on the cross-module
        // call. For an operator `mm.name` IS the operator token (`+`, `==`,
        // `mod`, …) — SymbolCollection at line 200 sets `sym->name = od->opToken`,
        // and the exporter carries it on the wire verbatim — so the
        // name-equals-mm.name check matches operators and methods the same way.
        for (const MetaMember& mm : td.members) {
            if (mm.kind != MetaMemberKind::Method || mm.methodBodyText.empty())
                continue;
            // Match by full parameter-type signature (the §22.6 overload set may
            // share a name; arity-only matching attaches the wrong body).
            sema::MethodSymbol* importedMs = nullptr;
            {
                const std::string mmSig = metaMemberSigKey("", mm.params);
                for (sema::Symbol* m : t->members) {
                    auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                    if (!ms) continue;
                    if (std::string(ms->name) != mm.name) continue;
                    if (ms->params.size() != mm.params.size()) continue;
                    std::string msSig = "(";
                    for (std::size_t pi = 0; pi < ms->params.size(); ++pi) {
                        if (pi) msSig += ',';
                        const auto* ps =
                            dynamic_cast<const sema::ParameterSymbol*>(
                                ms->params[pi]);
                        msSig += typeRefSigText(
                            ps ? ps->resolvedDeclType : nullptr);
                    }
                    msSig += ')';
                    if (msSig == mmSig) { importedMs = ms; break; }
                }
            }
            if (importedMs)
                parseAndAttachMethodBody(td, mm, importedMs, *t, ctx,
                    (t->kind == ast::TypeDeclKind::Class &&
                     td.genericParams.empty()),
                    md.moduleName);
        }

        // §14.1 / §23 / §2.8.2 / §2.8.3 rule 4 (fix #2893): property bodies —
        // find each Property member with a non-empty propertyBodyText and
        // attach the re-parsed ast::PropertyDecl to the matching imported
        // PropertySymbol. With the attached decl, codegen's
        // computedGetterOf / blockGetterOf / computedSetterOf /
        // blockSetterOf / lowerPropertyCompound paths engage (the same
        // paths the local generic-class property path uses, pinned by
        // #2884's own T8 control test) and monomorphize per instantiation
        // with genericSubst_ active.
        //
        // Properties are not overloadable (§22.6), so a name match is
        // sufficient. The loop runs AFTER the method loop so any sibling
        // method a property body references resolves against an
        // already-attached MethodDecl (the method path's own keep-alive
        // discipline applies in reverse: parseAndAttachPropertyBody keeps
        // the fresh context alive past this loop).
        //
        // SCOPE — PROPERTY only, not INDEXER. The cross-module INDEXER
        // path (#2946) exports an `bz$<Class>$idxget$<arity>` ExternalLinkage
        // symbol whose body is the erased `T -> i32` template accessor; the
        // itable fill (buildImportedGenericVtable in
        // LlvmBackend_Setup.cpp) installs that symbol in the iface slot.
        // Attaching a live `ast::PropertyDecl` here would (correctly) trip
        // the itable-fill's "local body" guard at line ~6584 of
        // LlvmBackend_Setup.cpp and leave the slot NULL — a regression of
        // the iface-indexer shape that #2946 fixed. Cross-module INDEXER
        // monomorphization is filed as a follow-up: the proper fix is the
        // same propertyBodyText carriage + a per-instantiation accessor
        // symbol the importer-side rewrite can substitute, parallel to
        // #3346/#3348's METHOD half. Until that lands, leave indexers
        // alone — the exported `bz$<Class>$idxget$<arity>` ABI (the
        // #2946 status quo) carries the layout-identical case, which is
        // the one #2946 pinned.
        for (const MetaMember& mm : td.members) {
            if (mm.kind != MetaMemberKind::Property ||
                mm.propertyBodyText.empty())
                continue;
            // Match by name (no signature, properties don't overload).
            sema::PropertySymbol* importedPs = nullptr;
            for (sema::Symbol* m : t->members) {
                auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                if (!ps) continue;
                if (ps->isIndexer) continue;
                if (std::string(ps->name) != mm.name) continue;
                importedPs = ps;
                break;
            }
            if (importedPs)
                parseAndAttachPropertyBody(td, mm, importedPs, *t, ctx);
        }

        // §2.8.2 / §22.12 / §22.13 (#2351): static field initializers — find
        // each Field member with a non-empty fieldInitText and attach the
        // re-parsed ast::FieldDecl to the matching imported FieldSymbol, so
        // codegen's genericInstSfinitFn can lower+store the initializer into
        // the #1513 shared per-instantiation cell. Fields are never
        // overloaded (§22.6 is a method/constructor/indexer concept), so a
        // name match is sufficient — no signature-key matching needed.
        //
        // fix #3736 (§9.2/§22.8/§2.8.2, v0.286): widened to ALSO drive a
        // Property member's fieldInitText (populated by
        // ModuleMetadataBuilder.cpp's PropertySymbol branch only for a
        // non-static AUTO-property of a generic type or a record — see that
        // branch's comment) — the Property twin of the instance-field path
        // #2874 already covers below. A Property's fieldInitText is NEVER
        // populated for a static member (the writer's gate is
        // `!isStaticProp` unconditionally), so only the non-static `else`
        // arm below is ever reached for a Property target; the `isStatic`
        // arm stays exactly as it was, reachable only for a Field target.
        for (const MetaMember& mm : td.members) {
            if (mm.fieldInitText.empty()) continue;
            if (mm.kind != MetaMemberKind::Field &&
                mm.kind != MetaMemberKind::Property)
                continue;
            sema::Symbol* importedMember = nullptr;
            if (mm.kind == MetaMemberKind::Property) {
                for (sema::Symbol* m : t->members) {
                    auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                    if (ps && !ps->isIndexer && std::string(ps->name) == mm.name) {
                        importedMember = ps;
                        break;
                    }
                }
            } else {
                for (sema::Symbol* m : t->members) {
                    auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
                    if (fs && std::string(fs->name) == mm.name) {
                        importedMember = fs;
                        break;
                    }
                }
            }
            if (!importedMember) continue;
            // fix #2860 (§2.8.2 "No restrictions beyond visibility", v0.240):
            // #2351's re-parse+attach is a STATIC-field mechanism — it sets
            // `FieldSymbol::decl`, which for an INSTANCE field is the key every
            // decl-null fallback in the backend keys the imported-member layout
            // path on (`storageType`'s `mem->decl == nullptr` guard above all),
            // so attaching one would silently re-route an imported generic's
            // instance layout through a TypeRef resolved in the throwaway
            // context.
            //
            // fix #2874 (§2.8.2 / §22.8 step 2, v0.240): the strictly better
            // outcome §2.8.2 wants — carry AND RUN an imported generic's
            // non-constant instance field initializer — IS now reached, by the
            // SAME parse-and-attach pipeline as #2351 but with the final
            // `importedFs->decl = parsedFd` step replaced by
            // `setImportedInstanceFieldInitExpr(importedFs, parsedFd->initializer)`,
            // leaving `decl` null so the imported-member layout fallbacks
            // continue to consult `importedMemberType` (storageType / #662).
            // A parse/sema failure is a NO-OP for the instance path
            // (a program that merely imports the offending generic still has
            // to build, §2.8.2); the construction-site diagnostic #2860 added
            // is the fallback, and the text in `importedInstanceFieldInitText`
            // drives it.
            bool isStaticMember = false;
            for (const std::string& mod : mm.modifiers)
                if (mod == "static") { isStaticMember = true; break; }
            if (isStaticMember) {
                parseAndAttachFieldInit(td, md.moduleName, mm, importedMember, *t,
                                        ctx, /*isStatic=*/true);
            } else {
                // ALWAYS record the source text: emitOwnFieldInit's existing
                // diagnostic path keys on it when neither the constant-fold
                // replay NOR the parsed-expr lowering produced a value
                // (parse failed / parse succeeded but lower returned null).
                // Cost: one std::string copy per non-static imported field.
                ctx.setImportedInstanceFieldInitText(importedMember,
                                                     mm.fieldInitText);
                // fix #2874: best-effort reconstruction. On success the
                // side-table expr drives emitOwnFieldInit's lower path; on
                // failure the construction-site diagnostic still fires.
                parseAndAttachFieldInit(td, md.moduleName, mm, importedMember, *t,
                                        ctx, /*isStatic=*/false);
            }
        }

        // Dtor body: the dtorBodyText is on the MetaTypeDecl itself.
        if (!td.dtorBodyText.empty()) {
            parseAndAttachDtorBody(td, td.dtorBodyText, *t, ctx);
        }

        // §22.8 step 1 / §22.10 / §11.1 (#3911/#3916): inheritance-clause
        // argument list — `baseCtorArgsText` is on the MetaTypeDecl itself.
        // Re-parse the argument text in a fresh SemaContext with a synthetic
        // class header (base type from `td.baseTypes[0]`, primary-ctor
        // parameters from `td.members` filtered to positional fields) so a
        // generic imported MIDDLE ancestor's OWN base clause can be replayed
        // by `emitImplicitBase` / `prePopulateBasePositionalFields`, which
        // both read `primaryBaseCtorArgs` via `dynamic_cast<ast::TypeDecl*>
        // (cls->decl)` — and `cls->decl` is null for every imported type
        // (the #2374-class gap; `TypeSymbol::decl` stays null on purpose —
        // widening it is a separate, much larger risk this fix declines to
        // take). The freshly-parsed `ast::CallArgument` vector is attached
        // to `t->importedPrimaryBaseCtorArgs`, exposed via
        // `primaryBaseCtorArgsOf` (Symbol.hpp).
        //
        // Gated on `hasPositionalFields` for symmetry with the exporter's
        // emission gate — a non-positional-ctor class has no
        // `primaryBaseCtorArgs` shape (the wire format's carriage is empty
        // for it), so this is a no-op rather than a malformed re-parse.
        if (t->hasPositionalFields && !td.baseCtorArgsText.empty()) {
            parseAndAttachBaseCtorArgs(td, *t, ctx);
        }
    }

    // §22.16 / §2.8.2 (fix #4592): Pass 3b — the class's OWN `invariant(E);`
    // clauses. `ownInvariantText` is on the MetaTypeDecl itself, like
    // `dtorBodyText` and `baseCtorArgsText`, but this walk is DELIBERATELY its
    // own loop rather than a step inside pass 3 above: pass 3's gate `continue`s
    // for every non-generic class that carries no exported method body, and a
    // class with an invariant is under no obligation to be generic or to export
    // a body — the two-module repro in #4592 (`public class BaseA … invariant(v
    // > 0);`) is exactly that shape, and attaching inside pass 3 left it
    // silently unreconstructed, i.e. the defect unchanged.
    //
    // Position after pass 3 matters for the same reason pass 3 runs after
    // pass 2: every member symbol a clause binds to (fields, auto-property
    // storage, `const`s, pure self-methods) must already exist on `*t`, or the
    // remap inside would find an empty member list and register the throwaway
    // context's own symbols — which codegen would then lower against a
    // different struct identity (the #3207 failure shape).
    //
    // fix #4599 residual 2 (§22.16 / §11.6, v0.306): the loop attaches a
    // TOP-LEVEL class's own clauses; `parseAndAttachInvariantsNested` below
    // additionally recurses through `td.members[].nestedType` so a §11.6
    // nested imported class's clauses are reached too (see that function's
    // header for why it is unconditional on `t->kind`).
    for (std::size_t i = 0; i < md.types.size(); ++i) {
        const MetaTypeDecl& td = md.types[i];
        sema::TypeSymbol* t = created[i];
        if (t->kind == ast::TypeDeclKind::Class && !td.ownInvariantText.empty())
            parseAndAttachInvariants(td, md.moduleName, *t, ctx);
        parseAndAttachInvariantsNested(td, md.moduleName, *t, ctx);
    }

    // §26 / §2.5 (#735) — Pass 4: reconstruct exported EXTENSION blocks. An
    // extension member belongs to no type, so it cannot ride the TypeSymbol
    // path: the importer needs a live `ast::ExtensionDecl` because
    // ExprTC::lookupExtensionMember matches on its `targetType` (§26.5 step 2)
    // and codegen's functionName / extensionThisType recover the exported
    // `bz$ext$<T>$<M>` symbol and its receiver ABI from the same node. So
    // synthesize a minimal AST: one ExtensionDecl per block, whose targetType is
    // a Named ast::Type with the block's head spelled from the metadata and the
    // resolved TypeRef registered directly. No re-parse and no fresh
    // SemaContext are needed (unlike the #662c/#783 generic BODY paths) — an
    // extension head is a plain type name, and the body never crosses the
    // boundary: the call links against the exporter's emitted symbol.
    if (!md.extensions.empty()) {
        auto synth = std::make_unique<ast::SourceFile>();
        for (const MetaExtension& me : md.extensions) {
            auto decl = std::make_unique<ast::ExtensionDecl>();
            decl->visibility = fromMetaVisibility(me.visibility);
            // §26.5 rule 4 (#2329): restore the exporter-stamped declaration
            // site on the synthetic node. `sourceFile` is separate because the
            // shared SourceSpan intentionally carries positions only.
            decl->sourceFile = me.sourceSpan.file;
            decl->span.begin.line = me.sourceSpan.beginLine;
            decl->span.begin.column = me.sourceSpan.beginColumn;
            decl->span.end.line = me.sourceSpan.endLine;
            decl->span.end.column = me.sourceSpan.endColumn;
            // The wire payload keeps each constraint as source-like text. Reuse
            // the language parser to reconstruct GenericConstraint exactly,
            // rather than duplicating A.3's type/constraint grammar here.
            if (!me.genericConstraints.empty()) {
                std::string constraintSource =
                    "module _2329_imported_extension_constraints; extension int ";
                for (const std::string& text : me.genericConstraints) {
                    constraintSource += text;
                    constraintSource += ' ';
                }
                constraintSource += "end;";
                DiagnosticEngine constraintDiag;
                Lexer constraintLexer(constraintSource, constraintDiag);
                Parser constraintParser(constraintLexer, constraintDiag);
                auto constraintAst = constraintParser.parseCompilationUnit();
                if (constraintAst && !constraintDiag.hasErrors()) {
                    for (auto& top : constraintAst->topDecls) {
                        auto* parsed =
                            dynamic_cast<ast::ExtensionDecl*>(top.get());
                        if (!parsed) continue;
                        decl->genericConstraints =
                            std::move(parsed->genericConstraints);
                        break;
                    }
                    // GenericConstraint strings and nested Type spellings are
                    // string_views into this buffer; keep it alive with the
                    // rest of the imported AST state.
                    ctx.ownImportedSource(std::move(constraintSource));
                }
            }
            auto targetTy = std::make_unique<ast::Type>();
            targetTy->kind = ast::TypeKind::Named;
            targetTy->name = std::make_unique<ast::QualifiedName>();
            // The spelled head must be the SAME last segment the exporter
            // mangled `bz$ext$<T>$<M>` with (Emitter::functionName reads
            // targetType->name->parts.back()), so a class/record/enum head uses
            // the type's own name and a §26.2 primitive head its keyword.
            targetTy->name->parts.push_back(
                ctx.internString(metaTypeToSourceText(me.target)));
            decl->targetType = std::move(targetTy);
            ctx.setResolvedType(decl->targetType.get(), loader.convert(me.target));

            auto* extSym = ctx.makeImportedSymbol<sema::ExtensionSymbol>();
            extSym->decl = decl.get();
            extSym->scope = sema::kModuleScope;

            // fix #3362 (§26.3 / §26.5 step 2 / §2.8.3 rule 4, v0.NNN,
            // importer half of #2583's writer-half fix): when the
            // extension's target is a class declared by this same
            // dep module, fetch its MetaTypeDecl + the importer-side
            // TypeSymbol so `parseAndAttachExtensionPropertyBody`
            // (below) can replay the property body in a fresh
            // SemaContext. A primitive target (e.g. `extension int`)
            // — or a target whose owning class lives in a DIFFERENT
            // dep module that this dep does not re-export —
            // receives `owningTd = nullptr` and the body re-parse
            // emits just the extension block (the body can only see
            // `this` in that case, which is sufficient for primitive
            // targets).
            const MetaTypeDecl* extOwningTd = nullptr;
            sema::TypeSymbol* extTargetTypeSym = nullptr;
            {
                const std::string targetSpelling =
                    metaTypeToSourceText(me.target);
                for (std::size_t ti = 0; ti < md.types.size(); ++ti) {
                    if (md.types[ti].name == targetSpelling) {
                        extOwningTd = &md.types[ti];
                        // `created` (per pass-1/2 layout) is parallel to
                        // md.types; the importer-side symbol for this
                        // type lives at the same index.
                        if (ti < created.size()) {
                            extTargetTypeSym = created[ti];
                        }
                        break;
                    }
                }
            }

            for (const MetaMember& mm : me.members) {
                if (mm.kind == MetaMemberKind::Property) {
                    // fix #3362 — parse-and-attach the property body.
                    // An extension property has NO external propget
                    // ABI (no `bz$ext$<T>$<P>$propget` symbol is ever
                    // emitted — extension properties are only ever
                    // inlined at the use site via computedGetterOf,
                    // which reads ps->decl->exprBody in codegen). For a
                    // cross-module extension property the importer has
                    // no local body to call, so the body MUST ride the
                    // wire and be re-parsed + attached here — the same
                    // shape as the §14.1/#2893 generic-class property
                    // body carriage, but for an extension. Mirrors
                    // parseAndAttachPropertyBody (above, the class
                    // twin); see that function's doc for the rationale
                    // (soundness gate, field remap, kept-alive source
                    // + AST + fresh ctx).
                    //
                    // The exporter side carried out at
                    // ModuleMetadataBuilder.cpp:2223 (Lane A #2583 +
                    // Lane G #3362 widening): convertMember's
                    // propertyBodyText gate accepts the extension
                    // branch via the new `wantPropertyBodyText=true`
                    // parameter, so `mm.propertyBodyText` is
                    // non-empty when the body span is well-formed and
                    // empty otherwise. An empty `propertyBodyText`
                    // means a degenerate span (the writer-side
                    // `begin.offset < end.offset && end.offset <=
                    // source.size()` gate failed) — fall through
                    // silently (the property still attaches as a
                    // no-body symbol, matching the class-side
                    // `parseAndAttachPropertyBody`'s own
                    // decl-null fallback when the wire text is
                    // empty).
                    if (!mm.propertyBodyText.empty()) {
                        auto* ps = ctx.makeImportedSymbol<sema::PropertySymbol>();
                        ps->name = ctx.internString(mm.name);
                        ps->visibility = fromMetaVisibility(mm.visibility);
                        ps->modifiers = loader.intern(mm.modifiers);
                        ps->scope = sema::kModuleScope;
                        ps->parent = nullptr;
                        ps->isIndexer = false;
                        // §26.3: extension properties are NEVER auto
                        // (the auto-body grammar is `property X: T;`
                        // / `property X: T := v;`, none of which
                        // §26.3 admits for an extension). Mark this
                        // explicitly so a downstream
                        // `isAutoPropertyStorageSymbol` cannot
                        // misclassify the synthetic PropertyDecl we
                        // are about to attach — exactly the safe
                        // pre-existing the class-side importer uses
                        // (#2478's side-table rider).
                        loader.propIsAutoStorage_[ps] = false;
                        // §23 / §2.8.2 / §2.8.3 rule 4: the property
                        // type rides on the loader's side table so
                        // codegen and sema can read it without an
                        // ast::PropertyDecl (none exists at this
                        // point). When the parse+attach below
                        // succeeds and `ps->decl` is non-null, both
                        // paths answer identically — the side table
                        // is the ONLY source of truth BEFORE the
                        // attach and codegen queries it once per
                        // use site anyway.
                        loader.fieldTypes_[ps] = loader.convert(mm.type);
                        // PropertySymbol exists; attempt to attach
                        // the parsed body. On success, `ps->decl` is
                        // set; on failure (re-parse / sema error), the
                        // property symbol exists with `ps->decl =
                        // nullptr` and reads return the codegen
                        // fallback (no-body computed read).
                        parseAndAttachExtensionPropertyBody(
                            me, extOwningTd, extTargetTypeSym, mm, ps, ctx);
                        extSym->members.push_back(ps);
                    }
                    continue;
                }
                if (mm.kind != MetaMemberKind::Method) continue;
                const sema::ScopeId scope = ctx.newScope();
                auto* ms = ctx.makeImportedSymbol<sema::MethodSymbol>();
                ms->name = ctx.internString(mm.name);
                ms->visibility = fromMetaVisibility(mm.visibility);
                ms->modifiers = loader.intern(mm.modifiers);
                // §26.1: an extension method has no `parent` TypeSymbol — that
                // null parent is exactly how codegen distinguishes it, together
                // with `extensionDecl`.
                ms->parent = nullptr;
                ms->scope = scope;
                ms->params = loader.convertParams(mm.params, scope);
                ms->extensionDecl = decl.get();
                ms->isPure = mm.isPure;
                ms->contractRecords = toContractRecords(mm.contracts);
                loader.returnTypes_[ms] = loader.convert(mm.type);
                loader.returnOwned_[ms] = mm.returnOwned;
                extSym->members.push_back(ms);
            }
            ctx.registerImportedExtension(md.moduleName, extSym);
            synth->topDecls.push_back(std::move(decl));
        }
        // The synthetic AST must outlive sema + codegen (every symbol above
        // points into it), so hand it to the context's imported-AST arena.
        ctx.ownImportedAst(std::move(synth));
    }

    // fix #3828 (§26.1 / §26.5 / §2.8.2 clause 1, v0.287): re-publish the
    // returnType / fieldType side tables HERE as well as at the early point
    // #3748 lifted them to. #3748 MOVED this block above pass 3 (pass 3's
    // field-init re-parse is the first consumer that reads these tables) and
    // deleted it from here — but pass 4, further down this same function, is
    // where an imported EXTENSION's members are built, and it writes
    // `loader.returnTypes_[ms]` for every extension METHOD (~line 4693) and
    // `loader.fieldTypes_[ps]` for every extension PROPERTY (~line 4665).
    // Everything pass 4 adds was therefore never published into the
    // SemaContext: an imported extension method's call site got no
    // `importedReturnType`, so its CallExpr carried no exprType, and the
    // §26.1 call degraded to a null lower — the #2434 / #2491 / #2961
    // "failed to lower to a value" warnings, with the enclosing local /
    // return / assignment silently zero-filled (measured: `x.Triple()` on an
    // extension-only dependency returned 0 instead of 21). Both publishes are
    // needed and the pair is safe: `setImportedReturnType` /
    // `setImportedMemberType` are idempotent map assignments over the same
    // (symbol, TypeRef) pairs, and `importedReturnOwned_` is an insert-only
    // set, so replaying the early entries here is a no-op and the pass-3/4
    // additions are picked up. Keep BOTH — deleting either reopens one of the
    // two defects.
    for (auto& [m, ret] : loader.returnTypes_)
        ctx.setImportedReturnType(m, ret, loader.returnOwned_[m]);
    for (auto& [m, ty] : loader.fieldTypes_)
        ctx.setImportedMemberType(m, ty);
    // fix #2391 (§14.2/§2.8.2, v0.236): publish imported generic base
    // methods' per-parameter constraint payload so checkOverrideSignatureMatch
    // can verify an override's restated clause set against an imported base.
    for (auto& [m, gcs] : loader.methodGenericConstraints_)
        ctx.setImportedGenericConstraints(m, std::move(gcs));
    // fix #2400 (§23/§2.8.2/§2.8.3 rule 4): publish imported property setter
    // presence so declareImportedPropertySetters (LlvmBackend_Setup.cpp) can
    // tell a genuinely settable imported property from a getter-only one.
    for (auto& [m, hs] : loader.propHasSetter_)
        ctx.setImportedPropertyHasSetter(m, hs);
    // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4): publish the
    // imported property/indexer setter's EFFECTIVE visibility so the `:=`
    // assignment-target check (ExprTypecheck.cpp) can enforce §13.6 across a
    // module boundary the same way it already does locally.
    for (auto& [m, sv] : loader.propSetterVisibility_)
        ctx.setImportedPropertySetterVisibility(m, sv);
    // fix #2478 (§9.2/§22.8): publish imported property AUTO-storage
    // classification so collectInstanceStorage/collectRecordStorage's
    // isAutoPropertyStorageSymbol fallback (sema/Symbol.hpp) can lay out an
    // imported type's storage correctly.
    for (auto& [m, isAuto] : loader.propIsAutoStorage_)
        ctx.setImportedPropertyIsAutoStorage(m, isAuto);
    // fix #2461 (§22.3/§23): publish imported property/indexer getter
    // presence so checkRefOutPropertyOperand (ExprTypecheck.cpp) can reject a
    // write-only imported property used as a `ref` argument with a clean
    // §22.3/§23 diagnostic (instead of falling through to a raw linker
    // error on the undefined `...$propget` symbol).
    for (auto& [m, hg] : loader.propHasGetter_)
        ctx.setImportedPropertyHasGetter(m, hg);
    // fix #2421 (§24/§2.8.2): publish imported event static/async
    // classification so the codegen event-dispatch path's eventIsStatic/
    // eventIsAsync helpers (LlvmBackend_impl.hpp) can classify an imported
    // event IDENTICALLY to the exporter — collectInstanceStorage/
    // emitOwnFieldInit/ctor-cleanup (isStatic) and the ifaceEventIndex_
    // mirror + itab$C$I build (isAsync), avoiding an itable/struct-layout
    // mismatch.
    for (auto& [m, st] : loader.eventIsStatic_)
        ctx.setImportedEventIsStatic(m, st);
    for (auto& [m, asy] : loader.eventIsAsync_)
        ctx.setImportedEventIsAsync(m, asy);
    // §22.12 (issue #328 FINDING [3] BUG A): publish imported const values so
    // codegen can fold an imported `const` read without a storage symbol.
    for (auto& [m, val] : loader.constValues_)
        ctx.setImportedConstValue(m, val);
}

bool loadMetadata(const ModuleMetadata& md, sema::SemaContext& ctx) {
    // Single-artifact load: the two passes run back-to-back over one module, so
    // the observable behaviour is byte-for-byte the pre-#1487 path. The
    // cross-artifact order-independence fix is loadDependencies, which runs pass 1
    // over EVERY dependency before pass 2 over any (see below).
    std::unordered_map<std::string, sema::TypeSymbol*> localTypes;
    std::vector<sema::TypeSymbol*> created;
    loadMetadataPass1(md, ctx, localTypes, created);
    loadMetadataPass2(md, ctx, localTypes, created);
    return true;
}

std::optional<ModuleMetadata> loadDependency(const std::string& artifactPath,
                                             sema::SemaContext& ctx,
                                             std::string* diag) {
    std::optional<std::vector<std::uint8_t>> bytes =
        readMetadataSection(artifactPath);
    if (!bytes) return std::nullopt;
    // §2.5 / §2.8.2 (issue #3198, v0.254): vintage check the producer
    // content-revision stamp BEFORE deserialize. A stale artifact (an old
    // build of this compiler, or a future build, or any artifact written
    // before the field landed) carries a stamp that does not equal
    // kProducerContentRevision — and the silent-acceptance shape the issue
    // measures (the consumer wrongly reads "owned" as missing, the layout
    // truncation, the disposal miscount) is exactly what would happen if
    // we let an old producer's bytes into the importer. The check is cheap
    // (a header read, no body decode) and rejects BEFORE any symbols are
    // imported into the SemaContext.
    std::optional<std::uint32_t> stamp = readProducerContentRevision(*bytes);
    if (!stamp) return std::nullopt;
    if (*stamp != kProducerContentRevision) {
        if (diag)
            *diag = "dependency artifact '" + artifactPath +
                    "' was built by a compiler with a different metadata "
                    "content revision (got " + std::to_string(*stamp) +
                    ", expected " + std::to_string(kProducerContentRevision) +
                    "); rebuild the dependency (§2.5 / §2.8.2)";
        return std::nullopt;
    }
    std::optional<ModuleMetadata> md = deserialize(*bytes);
    if (!md) return std::nullopt;
    // §2.8.4 rule 4 (issue #1733, v0.188): "Presenting an executable artifact as
    // a dependency is a build error naming the artifact and its kind." An
    // executable carries a native entry symbol; admitting it as a dependency
    // re-opens rule 3's entry-point perturbation hole from the other side. Reject
    // BEFORE loadMetadata imports any of its symbols.
    if (md->isExecutable) {
        if (diag)
            *diag = "dependency artifact '" + artifactPath +
                    "' is an executable, not a library (§2.8.4 rule 4)";
        return std::nullopt;
    }
    if (!loadMetadata(*md, ctx)) return std::nullopt;
    return md;
}

// §2.8.1 rule 5 (#1729, v0.188): the implementation-provided module set —
// the §2.3 always-available set (System, System.Text, System.Collections.Generic,
// System.Collections.Linq — the third joined the set in v0.289, proposal 0122
// L3, #3843) plus the explicitly-imported modules the standard-library document
// defines (System.Threading, System.IO, System.Diagnostics, System.Serialization,
// System.Reflection, System.Interop). Adding a name here without also editing
// the spec/stdlib amendment is a spec defect — see the proposal 0034 ruling and
// the file header in docs/proposals/0034-module-identity.md §1 rule 5.
static constexpr const char* kImplProvidedModuleNames[] = {
    "System",                       // stdlib §2
    "System.Collections.Generic",   // stdlib §4 (auto-imported, v0.289)
    "System.Collections.Linq",      // stdlib §5 (auto-imported, v0.278)
    "System.Diagnostics",           // stdlib §8
    "System.Interop",               // stdlib §10.5/§10.6
    "System.IO",                    // stdlib §7
    "System.Reflection",            // stdlib §11 (deferred, but reserved)
    "System.Serialization",         // stdlib §2.15
    "System.Text",                  // stdlib §3
    "System.Threading",             // stdlib §6
};

bool isImplProvidedModuleName(std::string_view name) {
    for (const char* implName : kImplProvidedModuleNames)
        if (name == implName) return true;
    return false;
}

bool loadDependencies(const std::vector<std::string>& artifactPaths,
                      sema::SemaContext& ctx, std::string* diag,
                      std::string* failedPath,
                      std::string_view rootModuleName,
                      std::vector<std::string>* survivingPaths) {
    // §2.5 / #1487: load a set of dependency artifacts so that a Named type
    // reference is resolved INDEPENDENTLY of the order they appear on the command
    // line. The old per-artifact loop (loadDependency once each, in sequence) did
    // pass 1+2 for artifact N before artifact N+1 even started, so a reference in
    // an earlier-loaded artifact to a user type declared in a later-loaded one
    // (e.g. LibM's `Maker.Make(): Payload` when Payload lives in LibB, loaded
    // second) missed by-name resolution and silently degraded to an
    // importedName-only TypeRef (declaration=nullptr) — the path meant for
    // genuinely-external builtins (§29.3 / #495) — yielding a garbage runtime
    // value. Reconstructing in two GLOBAL passes (all shells registered, then all
    // members filled) closes that gap while leaving the single-artifact and
    // builtin-import paths untouched.
    struct DepState {
        ModuleMetadata md;
        std::string path;
        std::unordered_map<std::string, sema::TypeSymbol*> localTypes;
        std::vector<sema::TypeSymbol*> created;
    };
    std::vector<DepState> states;
    states.reserve(artifactPaths.size());

    // Read + validate every dependency FIRST (so a bad/executable artifact is
    // rejected before any symbol is imported, exactly as loadDependency does).
    for (const std::string& path : artifactPaths) {
        std::optional<std::vector<std::uint8_t>> bytes =
            readMetadataSection(path);
        if (!bytes) {
            if (failedPath) *failedPath = path;
            return false;
        }
        // §2.5 / §2.8.2 (issue #3198, v0.254): vintage check via the producer
        // content-revision stamp (see loadDependency for the rationale). A
        // stale artifact is rejected before deserialize, exactly as a
        // bad-magic or executable artifact is.
        std::optional<std::uint32_t> stamp = readProducerContentRevision(*bytes);
        if (!stamp) {
            if (failedPath) *failedPath = path;
            return false;
        }
        if (*stamp != kProducerContentRevision) {
            if (diag)
                *diag = "dependency artifact '" + path +
                        "' was built by a compiler with a different metadata "
                        "content revision (got " + std::to_string(*stamp) +
                        ", expected " + std::to_string(kProducerContentRevision) +
                        "); rebuild the dependency (§2.5 / §2.8.2)";
            if (failedPath) *failedPath = path;
            return false;
        }
        std::optional<ModuleMetadata> md = deserialize(*bytes);
        if (!md) {
            if (failedPath) *failedPath = path;
            return false;
        }
        // §2.8.4 rule 4 (issue #1733): reject an executable presented as a
        // dependency (see loadDependency for the rationale).
        if (md->isExecutable) {
            if (diag)
                *diag = "dependency artifact '" + path +
                        "' is an executable, not a library (§2.8.4 rule 4)";
            if (failedPath) *failedPath = path;
            return false;
        }
        // §2.8.1 rule 5 / rule 3 (#1729, v0.188): the implementation-provided
        // modules participate in duplicate-name detection. A dependency
        // artifact declaring an impl-provided name is a build error — the
        // implementation's stdlib §X IS the only artifact that supplies that
        // name, and presenting a competing user artifact would let a file
        // silently override the stdlib's surface.
        if (isImplProvidedModuleName(md->moduleName)) {
            if (diag)
                *diag = "dependency artifact '" + path +
                        "' declares module '" + md->moduleName +
                        "', which is an implementation-provided module name "
                        "(§2.8.1 rules 3+5); stdlib modules cannot be "
                        "re-declared by user artifacts";
            if (failedPath) *failedPath = path;
            return false;
        }
        // §2.8.1 rule 3 (#1729, v0.188): "a dependency artifact declaring a
        // module name that the root compilation also declares" is a build
        // error. The root compilation owns its own module name; presenting a
        // dependency under the same name would silently widen visibility for
        // the root's `internal` access (§2.8.1 rule 2's closure-by-compilation
        // invariant). Distinct from the "two-deps-same-name" rule below
        // (root vs dep is the head of the build, dep vs dep is in closure).
        if (!rootModuleName.empty() &&
            md->moduleName == std::string(rootModuleName)) {
            if (diag)
                *diag = "dependency artifact '" + path +
                        "' declares module '" + md->moduleName +
                        "', which is also the name of the root compilation; "
                        "a build's dependency closure cannot redeclare a "
                        "module the root already supplies (§2.8.1 rule 3)";
            if (failedPath) *failedPath = path;
            return false;
        }
        // §2.8.3 rule 3 / §2.8.1 rule 3: repeated artifacts for one module
        // are idempotent only when their descriptions agree. This is the
        // metadata-time staleness backstop for carried contract records.
        auto prior = std::find_if(
            states.begin(), states.end(), [&](const DepState& state) {
                return state.md.moduleName == md->moduleName;
            });
        if (prior != states.end()) {
            if (!metadataDescriptionsAgree(prior->md, *md)) {
                if (diag)
                    *diag = "two distinct dependency artifacts declare the "
                            "same module name '" + md->moduleName +
                            "' ('" + prior->path + "' and '" + path +
                            "') but describe it differently; a build whose "
                            "artifact set contains two distinct artifacts "
                            "declaring one module name is ill-formed "
                            "(§2.8.1 rule 3)";
                if (failedPath) *failedPath = path;
                return false;
            }
            // Prefer a record-carrying rebuild over an opaque legacy copy; an
            // equal carried duplicate otherwise needs no second symbol shell.
            // (`prior->path` is updated so the survivingPaths list reports
            // the canonical copy — the one a downstream native link will
            // need, since the metadata-deduped copy never reaches the link.)
            if (!prior->md.contractRecordsCarried &&
                md->contractRecordsCarried) {
                prior->md = std::move(*md);
                prior->path = path;
            }
            continue;
        }
        DepState st;
        st.path = path;
        st.md = std::move(*md);
        states.push_back(std::move(st));
    }

    // Pass 1 across ALL dependencies: register every exported type's shell, so
    // the global by-name lookup in pass 2 sees the whole import set at once.
    for (DepState& st : states)
        loadMetadataPass1(st.md, ctx, st.localTypes, st.created);
    // Pass 2 across ALL dependencies: fill members/bases, now that every shell is
    // registered — a Named ref binds by name whatever the load order.
    for (DepState& st : states)
        loadMetadataPass2(st.md, ctx, st.localTypes, st.created);
    // §2.8.1 rule 3's idempotence carve-out (#1729, v0.188): bit-identical
    // duplicates are deduplicated, not diagnosed. The metadata side above
    // already collapses them (the `continue` above skips adding the second
    // copy to `states`); but the artifact PATH is still handed to the native
    // link step unless we tell the caller which paths survived. Without this
    // list, a diamond assembly (LibA + LibB both depend on Base, both ship
    // base.o on the command line) links two copies of every Base symbol and
    // fails with "multiple definition of …" — the link-time surface of the
    // very dedup the metadata side already performed.
    if (survivingPaths) {
        survivingPaths->clear();
        survivingPaths->reserve(states.size());
        for (const DepState& st : states)
            survivingPaths->push_back(st.path);
    }
    return true;
}

}  // namespace bezel::module
