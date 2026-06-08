#include "module/ModuleLoader.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "module/ModuleMetadataBuilder.hpp"  // kMetadataSectionName
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

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
        auto it = localTypes.find(name);
        if (it != localTypes.end()) return it->second;
        return nullptr;  // unresolved (generic param / external) — Named w/o decl
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
            out.push_back(g);
        }
        return out;
    }

    // Reconstruct one member onto `parent`. Returns the symbol (already added by
    // caller) or null for member kinds not reconstructed in this increment.
    sema::Symbol* convertMember(const MetaMember& mm, sema::TypeSymbol& parent) {
        const sema::ScopeId scope = ctx.newScope();
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
                auto* p = ctx.makeImportedSymbol<sema::PropertySymbol>();
                p->name = ctx.internString(mm.name);
                p->visibility = fromMetaVisibility(mm.visibility);
                p->modifiers = intern(mm.modifiers);
                p->parent = &parent;
                p->scope = parent.memberScope;
                p->isIndexer = false;
                fieldTypes_[p] = convert(mm.type);
                return p;
            }
            // Indexer / Event are part of the schema but deferred.
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
    // §22.12 imported const values (issue #328 FINDING [3] BUG A), textual.
    std::unordered_map<const sema::MemberSymbol*, std::string> constValues_;
};

}  // namespace

bool loadMetadata(const ModuleMetadata& md, sema::SemaContext& ctx) {
    std::unordered_map<std::string, sema::TypeSymbol*> localTypes;
    Loader loader{ctx, localTypes};

    // Pass 1: create a TypeSymbol shell per exported type so cross-references
    // (a member whose type is a sibling exported type) resolve by name (§2.5).
    std::vector<sema::TypeSymbol*> created;
    created.reserve(md.types.size());
    for (const MetaTypeDecl& td : md.types) {
        auto* t = ctx.makeImportedSymbol<sema::TypeSymbol>();
        t->name = ctx.internString(td.name);
        t->kind = fromMetaTypeDeclKind(td.kind);
        t->visibility = fromMetaVisibility(td.visibility);
        t->modifiers = loader.intern(td.modifiers);
        t->scope = sema::kModuleScope;
        t->memberScope = ctx.newScope();
        localTypes[td.name] = t;
        created.push_back(t);
    }

    // Pass 2: fill generic params, base types, and members (now that every
    // sibling TypeSymbol exists for by-name binding).
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
        for (const MetaMember& mm : td.members) {
            if (sema::Symbol* m = loader.convertMember(mm, *t))
                t->members.push_back(m);
        }
        ctx.registerImportedType(md.moduleName, t);
    }

    // Publish the loader's member-type side tables into the SemaContext so the
    // rest of the front-end (and codegen) can read an imported member's resolved
    // type without an ast::Type node.
    for (auto& [m, ret] : loader.returnTypes_)
        ctx.setImportedReturnType(m, ret, loader.returnOwned_[m]);
    for (auto& [m, ty] : loader.fieldTypes_)
        ctx.setImportedMemberType(m, ty);
    // §22.12 (issue #328 FINDING [3] BUG A): publish imported const values so
    // codegen can fold an imported `const` read without a storage symbol.
    for (auto& [m, val] : loader.constValues_)
        ctx.setImportedConstValue(m, val);

    return true;
}

std::optional<ModuleMetadata> loadDependency(const std::string& artifactPath,
                                             sema::SemaContext& ctx) {
    std::optional<std::vector<std::uint8_t>> bytes =
        readMetadataSection(artifactPath);
    if (!bytes) return std::nullopt;
    std::optional<ModuleMetadata> md = deserialize(*bytes);
    if (!md) return std::nullopt;
    if (!loadMetadata(*md, ctx)) return std::nullopt;
    return md;
}

}  // namespace bezel::module
