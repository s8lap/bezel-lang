#include "module/ModuleLoader.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
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
        // §9.2 (issue #501): propagate positional-record flag.
        t->hasPositionalFields = td.hasPositionalFields;
        t->genericParams = convertGenericParams(td.genericParams, t->memberScope);
        for (const MetaType& base : td.baseTypes) {
            sema::TypeRef* ref = convert(base);
            t->baseTypeRefs.push_back(ref);
            if (ref->kind == sema::TypeRef::Kind::Named && ref->declaration)
                t->baseTypes.push_back(ref->declaration);
        }
        for (const MetaMember& mm : td.members) {
            if (sema::Symbol* m = convertMember(mm, *t))
                t->members.push_back(m);
        }
        return t;
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
                return nts;
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
            return true;
        default:
            return false;  // Indexer (3), Event (5), and any future default
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

// §14/#662c / #695: build a stub declaration for an instance method whose body
// is not available.  The stub only carries the signature (enough for sema to
// register a MethodSymbol and resolve calls to it inside the ctor/dtor body).
// Limitations: operators, abstract methods, and methods with `Function`/
// `InlineArray` return types receive an `=> 0`-style stub; they should not
// normally be called from a ctor/dtor body, but we emit stubs anyway so that
// any call to them is at least resolved (rather than silently returning zero).
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
    // Minimal expression body.  Use a literal that matches the return type so
    // the stub compiles without type errors.  For pointer-sized types that
    // sema would flag as missing, fall back to `0`.
    if (hasReturnType) {
        std::string retText = metaTypeToSourceText(mm.type);
        bool isBool = (mm.type.kind == TypeKind::Primitive &&
                       mm.type.primitive == PrimitiveKind::Bool);
        s += " => " + std::string(isBool ? "false" : "0") + ";";
    } else {
        s += " end " + std::string(mm.name.empty() ? "unknown" : mm.name) + ";";
    }
    return s;
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
    // Class header with visibility and generic params.
    src += metaVisibilityToKeyword(td.visibility);
    src += " class ";
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

    // Run the three sema passes in a fresh SemaContext.
    DiagnosticEngine semaDiag;
    sema::SemaContext freshCtx(semaDiag);
    sema::collectSymbols(*sf, freshCtx);
    if (semaDiag.hasErrors()) return false;
    sema::resolveTypes(*sf, freshCtx);
    if (semaDiag.hasErrors()) return false;
    sema::typecheckExpressions(*sf, freshCtx);
    // Note: sema may emit errors for unsupported constructs (e.g. `base(...)`
    // chain on unknown base); tolerate those — we only need the exprSymbols.
    // A hard parse failure is caught above; sema errors here are usually
    // benign for the subset we're interested in (field assign + param refs).
    // Note: sema may emit errors for unsupported constructs (e.g. `base(...)`
    // chain on unknown base); tolerate those — we only need the exprSymbols.
    // A hard parse failure is caught above; sema errors here are usually
    // benign for the subset we're interested in (field assign + param refs).

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

    // Walk fresh context's exprSymbols and remap into mainCtx.
    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            // Field reference: find same-named field in importedType.
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (dynamic_cast<sema::ParameterSymbol*>(freshSym)) {
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
        }
        // Other symbols (TypeSymbol for type refs, etc.) are not remapped;
        // they are not needed by inlineCtorBody / implicitFieldAddr.
    }

    // Attach the parsed ConstructorDecl and keep the AST alive.
    // Keep the backing source string alive too: all string_view fields in the
    // re-parsed AST (IdentifierExpr::name, AssignmentStmt::op, …) point into
    // it — if it were destroyed on function return they would dangle.
    importedCs->decl = parsedCd;
    mainCtx.ownImportedSource(std::move(source));
    mainCtx.ownImportedAst(std::move(sf));
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

    DiagnosticEngine semaDiag;
    sema::SemaContext freshCtx(semaDiag);
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
        }
    }

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

    for (const auto& [expr, freshSym] : freshCtx.allExprSymbols()) {
        if (!expr || !freshSym) continue;
        if (dynamic_cast<sema::FieldSymbol*>(freshSym)) {
            auto it = importedFields.find(freshSym->name);
            if (it != importedFields.end())
                mainCtx.setExprSymbol(expr, it->second);
        } else if (dynamic_cast<sema::ParameterSymbol*>(freshSym)) {
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
    for (const auto& [astTy, ref] : freshCtx.allResolvedTypes())
        if (astTy && ref) mainCtx.setResolvedType(astTy, ref);
    for (const auto& [expr, ref] : freshCtx.allExprTypes())
        if (expr && ref) mainCtx.setExprType(expr, ref);

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
        // §9.2 (schema v4, issue #501): carry the positional-record flag so the
        // §9.2 auto-equality check in ExprTypecheck_Exprs.cpp reads it from
        // TypeSymbol::hasPositionalFields (no ast::TypeDecl exists for imported types).
        t->hasPositionalFields = td.hasPositionalFields;
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

    // Pass 3 (§14/#662c, schema v5): for imported generic CLASSES, re-parse
    // exported ctor/dtor body text and attach ast::ConstructorDecl /
    // ast::DestructorDecl to the reconstructed symbols so inlineCtorBody can
    // emit the body with the per-instantiation genericSubst_ active.
    for (std::size_t i = 0; i < md.types.size(); ++i) {
        const MetaTypeDecl& td = md.types[i];
        sema::TypeSymbol* t = created[i];
        // Only generic classes carry exported bodies.
        if (t->kind != ast::TypeDeclKind::Class || td.genericParams.empty())
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

        // Dtor body: the dtorBodyText is on the MetaTypeDecl itself.
        if (!td.dtorBodyText.empty()) {
            parseAndAttachDtorBody(td, td.dtorBodyText, *t, ctx);
        }
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

}  // namespace bezel::module
