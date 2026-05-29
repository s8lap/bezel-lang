#include "sema/TypeResolution.hpp"

#include <unordered_map>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

PrimitiveType primitiveFromName(std::string_view name) {
    if (name == "bool")    return PrimitiveType::Bool;
    if (name == "char")    return PrimitiveType::Char;
    if (name == "string")  return PrimitiveType::String;
    if (name == "byte")    return PrimitiveType::Byte;
    if (name == "sbyte")   return PrimitiveType::SByte;
    if (name == "short")   return PrimitiveType::Short;
    if (name == "ushort")  return PrimitiveType::UShort;
    if (name == "int")     return PrimitiveType::Int;
    if (name == "uint")    return PrimitiveType::UInt;
    if (name == "long")    return PrimitiveType::Long;
    if (name == "ulong")   return PrimitiveType::ULong;
    if (name == "float")   return PrimitiveType::Float;
    if (name == "double")  return PrimitiveType::Double;
    if (name == "decimal") return PrimitiveType::Decimal;
    if (name == "Object")  return PrimitiveType::Object;
    return PrimitiveType::None;
}

namespace {

// Lookup environment in scope. A stack of scopes (module-level + per-type
// + per-method). Each scope contributes a small name → Symbol table.
struct Scope {
    std::unordered_map<std::string_view, Symbol*> names;
    Scope* parent = nullptr;
};

// Walker for type-resolution. Threads a Scope* through the AST visit.
class Resolver {
public:
    Resolver(SemaContext& ctx) : ctx_(ctx) {}

    void run(const ast::SourceFile& src) {
        Scope moduleScope;
        // Populate module-scope: every top-level type / alias by name.
        if (ctx_.module()) {
            for (Symbol* child : ctx_.module()->children) {
                if (!child->name.empty()) {
                    moduleScope.names[child->name] = child;
                }
            }
        }
        for (const auto& decl : src.topDecls) {
            if (!decl) continue;
            resolveTopDecl(*decl, &moduleScope);
        }
    }

private:
    SemaContext& ctx_;

    void resolveTopDecl(const ast::TopDecl& decl, Scope* parent) {
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(&decl)) {
            resolveTypeDecl(*td, parent);
        } else if (auto* alias = dynamic_cast<const ast::TypeAlias*>(&decl)) {
            Scope aliasScope;
            aliasScope.parent = parent;
            for (const auto& g : alias->genericParams) {
                // Register a temporary symbol for lookup; we don't need to
                // create a TypeSymbol stand-in because the alias body only
                // ever references the name.
                aliasScope.names[g.name] = nullptr; // marker for "is generic"
            }
            if (alias->aliasedType) {
                resolveType(*alias->aliasedType, &aliasScope);
            }
            for (const auto& gc : alias->genericConstraints) {
                for (const auto& item : gc.items) {
                    if (item.typeArg) resolveType(*item.typeArg, &aliasScope);
                }
            }
        } else if (auto* ext =
                       dynamic_cast<const ast::ExtensionDecl*>(&decl)) {
            Scope extScope;
            extScope.parent = parent;
            if (ext->targetType) resolveType(*ext->targetType, &extScope);
            for (const auto& gc : ext->genericConstraints) {
                for (const auto& item : gc.items) {
                    if (item.typeArg) resolveType(*item.typeArg, &extScope);
                }
            }
            for (const auto& m : ext->members) {
                if (m) resolveMember(*m, &extScope);
            }
        } else if (auto* xb =
                       dynamic_cast<const ast::ExternBlock*>(&decl)) {
            for (const auto& d : xb->methods) {
                for (const auto& p : d.params) {
                    if (p.type) resolveType(*p.type, parent);
                }
                if (d.returnType) resolveType(*d.returnType, parent);
            }
        }
    }

    void resolveTypeDecl(const ast::TypeDecl& td, Scope* parent) {
        Scope typeScope;
        typeScope.parent = parent;
        // Register the type's generic params + any nested types so members
        // can reference them.
        for (const auto& g : td.genericParams) {
            typeScope.names[g.name] = nullptr;  // marker for generic-param
        }
        for (const auto& m : td.members) {
            if (auto* nt =
                    dynamic_cast<const ast::NestedTypeMember*>(m.get())) {
                if (nt->decl) {
                    // Look up the corresponding TypeSymbol by name in the
                    // outer type's members — slice 1 stored them already.
                    // For now we just register the name as visible.
                    typeScope.names[nt->decl->name] = nullptr;
                }
            }
        }
        // Base types.
        for (const auto& bt : td.baseTypes) {
            if (bt) resolveType(*bt, &typeScope);
        }
        // Generic constraints.
        for (const auto& gc : td.genericConstraints) {
            for (const auto& item : gc.items) {
                if (item.typeArg) resolveType(*item.typeArg, &typeScope);
            }
        }
        // Positional fields (records).
        for (const auto& pf : td.positionalFields) {
            if (pf.type) resolveType(*pf.type, &typeScope);
        }
        // SerializedClassClause polymorphic case types.
        if (td.serializedClass) {
            for (const auto& pc : td.serializedClass->polyCases) {
                if (pc.type) resolveType(*pc.type, &typeScope);
            }
        }
        // Members.
        for (const auto& m : td.members) {
            if (m) resolveMember(*m, &typeScope);
        }
        // Enum constants don't carry types syntactically (their `:= value`
        // is a Const expression; types come from the underlying-int).
    }

    void resolveMember(const ast::Member& m, Scope* parent) {
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
            if (fd->type) resolveType(*fd->type, parent);
        } else if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
            Scope methodScope;
            methodScope.parent = parent;
            for (const auto& g : md->genericParams) {
                methodScope.names[g.name] = nullptr;
            }
            for (const auto& p : md->params) {
                if (p.type) resolveType(*p.type, &methodScope);
            }
            if (md->returnType) resolveType(*md->returnType, &methodScope);
            for (const auto& gc : md->genericConstraints) {
                for (const auto& item : gc.items) {
                    if (item.typeArg) resolveType(*item.typeArg, &methodScope);
                }
            }
            // Body expressions/statements not visited in slice 2.
        } else if (auto* od =
                       dynamic_cast<const ast::OperatorDecl*>(&m)) {
            Scope methodScope;
            methodScope.parent = parent;
            for (const auto& p : od->params) {
                if (p.type) resolveType(*p.type, &methodScope);
            }
            if (od->returnType) resolveType(*od->returnType, &methodScope);
        } else if (auto* cd =
                       dynamic_cast<const ast::ConstructorDecl*>(&m)) {
            Scope ctorScope;
            ctorScope.parent = parent;
            for (const auto& p : cd->params) {
                if (p.type) resolveType(*p.type, &ctorScope);
            }
        } else if (auto* pd =
                       dynamic_cast<const ast::PropertyDecl*>(&m)) {
            if (pd->type) resolveType(*pd->type, parent);
            for (const auto& p : pd->indexerParams) {
                if (p.type) resolveType(*p.type, parent);
            }
        } else if (auto* ed = dynamic_cast<const ast::EventDecl*>(&m)) {
            if (ed->type) resolveType(*ed->type, parent);
        } else if (auto* cd2 = dynamic_cast<const ast::ConstDecl*>(&m)) {
            if (cd2->type) resolveType(*cd2->type, parent);
        } else if (auto* nt =
                       dynamic_cast<const ast::NestedTypeMember*>(&m)) {
            if (nt->decl) resolveTypeDecl(*nt->decl, parent);
        }
    }

    Symbol* lookupName(std::string_view name, Scope* scope) {
        for (Scope* s = scope; s; s = s->parent) {
            auto it = s->names.find(name);
            if (it != s->names.end()) return it->second;
        }
        return nullptr;
    }

    TypeRef* resolveType(const ast::Type& t, Scope* scope) {
        auto* ref = ctx_.makeTypeRef();
        ref->astNode = &t;
        ref->nullable = t.nullable;
        switch (t.kind) {
            case ast::TypeKind::Unit:
                ref->kind = TypeRef::Kind::Unit;
                break;
            case ast::TypeKind::InlineArray:
                ref->kind = TypeRef::Kind::InlineArray;
                ref->arraySize = t.arraySize.get();
                ref->arrayOwned = t.arrayOwned;
                if (t.arrayElement) {
                    ref->arrayElement = resolveType(*t.arrayElement, scope);
                }
                break;
            case ast::TypeKind::Function:
                ref->kind = TypeRef::Kind::Function;
                ref->funcReturnOwned = t.funcReturnOwned;
                for (const auto& p : t.funcParams) {
                    TypeRef::FuncParam fp;
                    fp.modifier = p.modifier;
                    fp.name = p.name;
                    if (p.type) fp.type = resolveType(*p.type, scope);
                    ref->funcParams.push_back(std::move(fp));
                }
                if (t.funcReturn) {
                    ref->funcReturn = resolveType(*t.funcReturn, scope);
                }
                break;
            case ast::TypeKind::Named:
                resolveNamed(t, scope, *ref);
                break;
        }
        ctx_.setResolvedType(&t, ref);
        return ref;
    }

    void resolveNamed(const ast::Type& t, Scope* scope, TypeRef& ref) {
        if (!t.name || t.name->parts.empty()) {
            ref.kind = TypeRef::Kind::Error;
            return;
        }
        std::string_view first = t.name->parts.front();
        // Primitive check first.
        if (t.name->parts.size() == 1) {
            PrimitiveType prim = primitiveFromName(first);
            if (prim != PrimitiveType::None) {
                ref.kind = TypeRef::Kind::Primitive;
                ref.primitive = prim;
                // Primitives shouldn't have generic args (sema enforces);
                // we still resolve them so the AST is fully visited.
                for (const auto& ga : t.genericArgs) {
                    if (ga) resolveType(*ga, scope);
                }
                if (!t.genericArgs.empty()) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        "primitive type cannot take generic arguments");
                }
                return;
            }
        }
        // Look up the first part.
        Symbol* sym = lookupName(first, scope);
        if (!sym) {
            // Unknown — but a null lookup may also mean "name is in scope as
            // a generic-param marker" (we used nullptr to indicate that).
            // Distinguish by checking if the key exists.
            bool isGeneric = false;
            for (Scope* s = scope; s; s = s->parent) {
                if (s->names.count(first)) { isGeneric = true; break; }
            }
            if (isGeneric) {
                // Generic parameter — record as a Named with no declaration.
                // Sema slice 3+ will treat generic-param TypeRefs specially.
                ref.kind = TypeRef::Kind::Named;
                ref.declaration = nullptr;
                return;
            }
            ctx_.diagnostics().report(
                Severity::Error, t.span.begin,
                std::string("unresolved type name `") +
                std::string(first) + "`");
            ref.kind = TypeRef::Kind::Error;
            return;
        }
        // For multi-part QualifiedNames (`Color.Red` style), we'd descend
        // into the looked-up symbol's members. Slice 2 supports only the
        // single-identifier case and a one-deep dotted name (e.g.
        // `System.Object` resolving to Object). For now we accept the
        // first part and ignore the rest if it's a TypeSymbol (the only
        // dotted-name case sema slice 2 needs is `Module.Name`-style).
        auto* tsym = dynamic_cast<TypeSymbol*>(sym);
        if (!tsym) {
            // Could be a TypeAliasSymbol or other. For slice 2, treat as
            // a Named pointing to no declaration (sema slice 3+ resolves
            // aliases).
            ref.kind = TypeRef::Kind::Named;
            ref.declaration = nullptr;
        } else {
            ref.kind = TypeRef::Kind::Named;
            ref.declaration = tsym;
            // Generic-arity check.
            if (!t.genericArgs.empty() &&
                t.genericArgs.size() != tsym->genericParams.size()) {
                ctx_.diagnostics().report(
                    Severity::Error, t.span.begin,
                    "generic-argument count does not match type's "
                    "generic-parameter count");
            }
        }
        for (const auto& ga : t.genericArgs) {
            if (!ga) continue;
            TypeRef* arg = resolveType(*ga, scope);
            ref.genericArgs.push_back(arg);
        }
    }
};

} // namespace

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx) {
    Resolver r(ctx);
    r.run(src);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
