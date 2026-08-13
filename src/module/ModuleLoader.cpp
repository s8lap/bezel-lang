#include "module/ModuleLoader.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lexer/Lexer.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"  // kMetadataSectionName
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
                for (const MetaType& ga : mt.genericArgs)
                    ref->genericArgs.push_back(convert(ga));
                break;
            }
            case TypeKind::InlineArray:
                ref->kind = sema::TypeRef::Kind::InlineArray;
                ref->arrayOwned = mt.arrayOwned;
                if (!mt.children.empty())
                    ref->arrayElement = convert(mt.children.front());
                // arraySize (a const expr) is not reconstructed — see the .hpp
                // deferred note; an imported inline-array field/return is a
                // declaration the importer can name but not yet lower.
                break;
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
            node->name->parts.push_back(ctx.internString(mt.namedName));
        }
        ctx.setResolvedType(node.get(), convert(mt));
        return node;
    }

    // Reconstruct a full TypeSymbol from a MetaTypeDecl (used for nested types,
    // §11.3 / issue #502). The reconstructed TypeSymbol lives in `parent`'s
    // member scope and has its own fresh member scope for its own members.
    sema::TypeSymbol* reconstructTypeSymbol(const MetaTypeDecl& td,
                                            sema::TypeSymbol& parent) {
        auto* t = ctx.makeImportedSymbol<sema::TypeSymbol>();
        t->name = ctx.internString(td.name);
        t->kind = fromMetaTypeDeclKind(td.kind);
        t->visibility = fromMetaVisibility(td.visibility);
        t->modifiers = intern(td.modifiers);
        t->scope = parent.memberScope;
        t->memberScope = ctx.newScope();
        // #1488: a nested type declares in the same module as its enclosing
        // type (§11.6 — nesting is not a separate module), so it inherits the
        // parent's already-populated moduleName rather than needing its own
        // MetaModule lookup.
        t->moduleName = parent.moduleName;
        // §9.2 (issue #501): propagate positional-record flag.
        t->hasPositionalFields = td.hasPositionalFields;
        // fix #2950 (§12 / §31.4 / §2.8.3 rule 4, v0.240): restore the built-in
        // exception base. Without it this reconstructed symbol answers "no
        // built-in base" to every layout consumer and the §31.4 header is
        // dropped on the importing side only.
        t->builtinExceptionBase = td.builtinExceptionBase;
        t->invariantContracts = toContractRecords(td.invariants);
        t->genericParams = convertGenericParams(td.genericParams, t->memberScope);
        for (const MetaType& base : td.baseTypes) {
            sema::TypeRef* ref = convert(base);
            t->baseTypeRefs.push_back(ref);
            if (ref->kind == sema::TypeRef::Kind::Named && ref->declaration)
                t->baseTypes.push_back(ref->declaration);
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
                // `sema::isAutoPropertySymbol` always returns false here
                // (no ast::PropertyDecl crosses the boundary).
                propIsAutoStorage_[p] = mm.isAutoProperty;
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

// Render a MetaType as Bezel source text for use in a synthetic class source.
static std::string metaTypeToSourceText(const MetaType& mt) {
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
            default: return "int";
        }
    }
    if (mt.kind == TypeKind::Named) {
        std::string s = mt.namedName;
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
    // Fallback for unsupported shapes (Function, InlineArray): use a placeholder.
    return "Object";
}

// §14/#705: canonical type text for a MetaType, used as a signature key for
// overload-safe matching (not for source-code generation — that is
// metaTypeToSourceText).  Uses ',' without space for generic args, and omits
// the nullable '?' suffix: stubs do not reliably preserve nullability for
// primitive types, and nullable-vs-non-nullable overloads are rare; the key's
// purpose is to distinguish base types, not nullability.
static std::string metaTypeSigText(const MetaType& mt) {
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
        std::string s = mt.namedName;
        if (!mt.genericArgs.empty()) {
            s += '<';
            for (std::size_t i = 0; i < mt.genericArgs.size(); ++i) {
                if (i) s += ',';
                s += metaTypeSigText(mt.genericArgs[i]);
            }
            s += '>';
        }
        // Nullable omitted intentionally (see comment above).
        return s;
    }
    return "?";  // Unit, Function, InlineArray: placeholder
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
// param type produces the same key as the MetaParam it was generated from.
// Only Named (which covers both user-declared names and primitive keywords)
// is handled; other shapes return "?" (a no-match sentinel).
static std::string astTypeToSigText(const ast::Type* t) {
    if (!t) return "?";
    if (t->kind == ast::TypeKind::Unit) return "()";
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
        // Nullable omitted (mirrors metaTypeSigText).
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
// ParameterSymbol (used for ctor-body attachment matching in facet b).
static std::string typeRefSigText(const sema::TypeRef* tr) {
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
            // Nullable omitted (mirrors metaTypeSigText).
            return s;
        }
        default:
            return "?";  // Error, InlineArray, Function: placeholder
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
static std::string buildMethodStub(const MetaMember& mm) {
    std::string s;
    // Visibility and non-body modifiers (skip abstract — it suppresses the body).
    bool isAbstract = false;
    for (const std::string& mod : mm.modifiers)
        if (mod == "abstract") isAbstract = true;
    if (isAbstract) return s;  // abstract methods have no body; skip stub
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
    // Fields (needed so implicit field references resolve).
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Field) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " " + mm.name + " : " + metaTypeToSourceText(mm.type) + ";\n";
    }
    // §14/#662c / #695: stub method declarations so self-calls in the ctor body
    // resolve to a MethodSymbol in the fresh context (remapped to the imported
    // MethodSymbol by parseAndAttachCtorBody).  Operator methods are skipped —
    // they use a different declaration syntax; they are unlikely to be called
    // explicitly from a ctor body and do not need stub resolution.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Method) continue;
        if (mm.isOperator) continue;
        std::string stub = buildMethodStub(mm);
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
        if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            // Field reference: find same-named field in importedType.
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (dynamic_cast<sema::ParameterSymbol*>(freshSym)) {
            // fix #2839: a LAMBDA parameter is declared inside the body and has
            // no importer-side counterpart (like a body-local, §2290 bug 1
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
            // §2290 bug 1: a LOCAL variable declared inside the ctor body
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

    // §2290 bug 1 / #783 precedent: copy the body's resolved-type and
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
        std::string stub = buildMethodStub(mm);
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
        if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
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

// §14.1 / #783: build a synthetic class source for re-parsing ONE imported
// generic method's body.  All fields are declared (so field references resolve),
// every OTHER method is emitted as a signature-only stub (so self-calls resolve),
// and `target`'s full declaration text is emitted verbatim in place of its stub.
// The returned source is identical in shape to buildSyntheticClassSource so the
// remap discipline (field/param/self-method) carries over unchanged.
static std::string buildSyntheticMethodSource(const MetaTypeDecl& td,
                                              const MetaMember& target) {
    std::string src = "module _783_reparsed_method_;\n";
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
    // Fields (needed so implicit field references resolve).
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Field) continue;
        src += "  " + metaVisibilityToKeyword(mm.visibility);
        for (const std::string& mod : mm.modifiers) src += " " + mod;
        src += " " + mm.name + " : " + metaTypeToSourceText(mm.type) + ";\n";
    }
    // Methods: the target carries its real body; every other method is a stub.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Method) continue;
        if (mm.isOperator) continue;
        if (&mm == &target) {
            src += "  " + target.methodBodyText + "\n";
        } else {
            std::string stub = buildMethodStub(mm);
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
    sema::SemaContext& mainCtx) {
    std::string source = buildSyntheticMethodSource(td, target);

    DiagnosticEngine freshDiag;
    Lexer freshLex(source, freshDiag);
    Parser freshParser(freshLex, freshDiag);
    auto sf = freshParser.parseCompilationUnit();
    if (!sf || freshDiag.hasErrors()) return false;

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
        if (ms && !ms->isOperator &&
            freshMethodSigKey(ms) == targetKey) { freshMs = ms; break; }
    }
    if (!freshMs) return false;

    auto* parsedMd = dynamic_cast<const ast::MethodDecl*>(freshMs->decl);
    if (!parsedMd) return false;

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

    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end())
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
            if (it != importedMethods.end())
                mainCtx.setExprSymbol(expr, it->second);
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
    importedMs->decl = parsedMd;
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
static std::string buildSyntheticFieldInitSource(const MetaTypeDecl& td,
                                                 const std::string& moduleName) {
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
    // Method stubs, for any self-call the initializer makes.
    for (const MetaMember& mm : td.members) {
        if (mm.kind != MetaMemberKind::Method) continue;
        if (mm.isOperator) continue;
        std::string stub = buildMethodStub(mm);
        if (!stub.empty()) src += stub + "\n";
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
static bool parseAndAttachFieldInit(
    const MetaTypeDecl& td,
    const std::string& moduleName,
    const MetaMember& target,
    sema::FieldSymbol* importedFs,
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

    sema::FieldSymbol* freshFs = nullptr;
    for (sema::Symbol* m : freshType->members) {
        auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
        if (fs && std::string(fs->name) == target.name) { freshFs = fs; break; }
    }
    if (!freshFs) {
        reject("the reconstructed field could not be located");
        return false;
    }

    auto* parsedFd = dynamic_cast<const ast::FieldDecl*>(freshFs->decl);
    if (!parsedFd || !parsedFd->initializer) {
        reject("the reconstructed declaration carries no initializer");
        return false;
    }

    // Field remap map (every field of the class, so a sibling reference in
    // `target`'s initializer binds to the REAL imported FieldSymbol).
    std::unordered_map<std::string_view, sema::Symbol*> importedFields;
    for (sema::Symbol* m : importedType.members) {
        if (dynamic_cast<sema::FieldSymbol*>(m) && !m->name.empty())
            importedFields[m->name] = m;
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
        if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end()) {
                mainCtx.setExprSymbol(expr, it->second);  // self-field → imported
                continue;
            }
            // #2375: not a self-field — a SIBLING type's field (e.g.
            // `Box.DefaultSize` in the reconstructed initializer). The
            // sibling was registered as the REAL imported TypeSymbol, so
            // freshSym already IS the imported FieldSymbol — copy it as-is.
            // Skipping it (the pre-#2375 behavior for any non-self symbol)
            // left codegen with no symbol for the reference and it silently
            // read zero.
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

    // Attach: importedFs->decl now points at a live ast::FieldDecl whose
    // `initializer` genericInstSfinitFn's existing loop lowers and stores.
    //
    // fix #2874 (§2.8.2 / §22.8 step 2, v0.240): for an INSTANCE field,
    // `decl` MUST stay null — `decl == nullptr` is the discriminator every
    // imported-member fallback in the backend keys on (storageType /
    // recordFieldType / isOwnedReferenceField / …). Instead, publish the
    // parsed initializer into the side table that emitOwnFieldInit reads
    // ahead of its constant-foldable replay and its construction-site
    // diagnostic fallback. The Expr* rides in `sf` (kept alive via
    // ownImportedAst below), so the side-table pointer is valid for the
    // lifetime of this SemaContext.
    if (isStatic) {
        importedFs->decl = parsedFd;
    } else {
        if (parsedFd->initializer)
            mainCtx.setImportedInstanceFieldInitExpr(importedFs,
                                                     parsedFd->initializer.get());
    }
    mainCtx.ownImportedSource(std::move(source));
    mainCtx.ownImportedAst(std::move(sf));
    mainCtx.ownImportedAuxiliary(semaDiag);
    mainCtx.ownImportedAuxiliary(freshCtxPtr);
    return true;
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
    }
}

void stripContractFacts(MetaTypeDecl& type) {
    type.invariants.clear();
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
        for (const MetaType& base : td.baseTypes) {
            sema::TypeRef* ref = loader.convert(base);
            t->baseTypeRefs.push_back(ref);
            if (ref->kind == sema::TypeRef::Kind::Named && ref->declaration)
                t->baseTypes.push_back(ref->declaration);
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

    // Pass 3 (§14/#662c, schema v5): for imported generic CLASSES, re-parse
    // exported ctor/dtor body text and attach ast::ConstructorDecl /
    // ast::DestructorDecl to the reconstructed symbols so inlineCtorBody can
    // emit the body with the per-instantiation genericSubst_ active.
    for (std::size_t i = 0; i < md.types.size(); ++i) {
        const MetaTypeDecl& td = md.types[i];
        sema::TypeSymbol* t = created[i];
        // Only generic classes and records carry exported constructor bodies.
        if ((t->kind != ast::TypeDeclKind::Class &&
             t->kind != ast::TypeDeclKind::Record) ||
            (t->kind == ast::TypeDeclKind::Class && td.genericParams.empty()))
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
                    if (!ms || ms->isOperator) continue;
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
                parseAndAttachMethodBody(td, mm, importedMs, *t, ctx);
        }

        // §2.8.2 / §22.12 / §22.13 (#2351): static field initializers — find
        // each Field member with a non-empty fieldInitText and attach the
        // re-parsed ast::FieldDecl to the matching imported FieldSymbol, so
        // codegen's genericInstSfinitFn can lower+store the initializer into
        // the #1513 shared per-instantiation cell. Fields are never
        // overloaded (§22.6 is a method/constructor/indexer concept), so a
        // name match is sufficient — no signature-key matching needed.
        for (const MetaMember& mm : td.members) {
            if (mm.kind != MetaMemberKind::Field || mm.fieldInitText.empty())
                continue;
            sema::FieldSymbol* importedFs = nullptr;
            for (sema::Symbol* m : t->members) {
                auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
                if (fs && std::string(fs->name) == mm.name) {
                    importedFs = fs;
                    break;
                }
            }
            if (!importedFs) continue;
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
                parseAndAttachFieldInit(td, md.moduleName, mm, importedFs, *t,
                                        ctx, /*isStatic=*/true);
            } else {
                // ALWAYS record the source text: emitOwnFieldInit's existing
                // diagnostic path keys on it when neither the constant-fold
                // replay NOR the parsed-expr lowering produced a value
                // (parse failed / parse succeeded but lower returned null).
                // Cost: one std::string copy per non-static imported field.
                ctx.setImportedInstanceFieldInitText(importedFs,
                                                     mm.fieldInitText);
                // fix #2874: best-effort reconstruction. On success the
                // side-table expr drives emitOwnFieldInit's lower path; on
                // failure the construction-site diagnostic still fires.
                parseAndAttachFieldInit(td, md.moduleName, mm, importedFs, *t,
                                        ctx, /*isStatic=*/false);
            }
        }

        // Dtor body: the dtorBodyText is on the MetaTypeDecl itself.
        if (!td.dtorBodyText.empty()) {
            parseAndAttachDtorBody(td, td.dtorBodyText, *t, ctx);
        }
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
            for (const MetaMember& mm : me.members) {
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

    // Publish the loader's member-type side tables into the SemaContext so the
    // rest of the front-end (and codegen) can read an imported member's resolved
    // type without an ast::Type node.
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
    // isAutoPropertyStorage fallback (LlvmBackend_impl.hpp) can lay out an
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

bool loadDependencies(const std::vector<std::string>& artifactPaths,
                      sema::SemaContext& ctx, std::string* diag,
                      std::string* failedPath) {
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
                    *diag = "stale or disagreeing descriptions for module '" +
                            md->moduleName + "' in dependency artifacts '" +
                            prior->path + "' and '" + path +
                            "' (§2.8.3 rule 3; recompile dependents)";
                if (failedPath) *failedPath = path;
                return false;
            }
            // Prefer a record-carrying rebuild over an opaque legacy copy; an
            // equal carried duplicate otherwise needs no second symbol shell.
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
    return true;
}

}  // namespace bezel::module
