#include "sema/SymbolCollection.hpp"

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"

namespace bezel::sema {

namespace {

// Build the dotted module name into a string_view backed by the AST's
// QualifiedName parts. Since QualifiedName::parts is a vector of
// string_views into the source, the parts already share the source's
// lifetime; we just join them lazily for diagnostic messages.
//
// For the canonical *symbol name* of a module, we use the first part —
// the leaf — to keep lookups predictable. The full dotted name lives on
// the ModuleDecl AST node and is recoverable from there.
std::string_view moduleSymbolName(const ast::QualifiedName* qn) {
    if (!qn || qn->parts.empty()) return {};
    return qn->parts.back();
}

// Forward declarations for the recursive descent.
Symbol* collectTopDecl(const ast::TopDecl& decl, SemaContext& ctx,
                       ScopeId parentScope);
TypeSymbol* collectTypeDecl(const ast::TypeDecl& td, SemaContext& ctx,
                            ScopeId parentScope);
Symbol* collectMember(const ast::Member& m, TypeSymbol& parent,
                      SemaContext& ctx);

// A.3 GenericParam: register one symbol per declared parameter in the
// given scope.
std::vector<Symbol*> collectGenericParams(
    const std::vector<ast::GenericParam>& params,
    SemaContext& ctx, ScopeId scope) {
    std::vector<Symbol*> out;
    out.reserve(params.size());
    for (const auto& p : params) {
        auto* sym = ctx.makeSymbol<GenericParamSymbol>();
        sym->name = p.name;
        sym->variance = p.variance;
        sym->scope = scope;
        out.push_back(sym);
    }
    return out;
}

// A.4 Param list: register one symbol per declared parameter.
std::vector<Symbol*> collectParams(const std::vector<ast::Param>& params,
                                   SemaContext& ctx, ScopeId scope) {
    std::vector<Symbol*> out;
    out.reserve(params.size());
    for (const auto& p : params) {
        auto* sym = ctx.makeSymbol<ParameterSymbol>();
        sym->name = p.name;
        sym->modifier = p.modifier;
        sym->scope = scope;
        sym->declType = p.type.get();
        out.push_back(sym);
    }
    return out;
}

Symbol* collectMember(const ast::Member& m, TypeSymbol& parent,
                      SemaContext& ctx) {
    // Each member has its own scope (parameters, locals live inside).
    const ScopeId scope = ctx.newScope();
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
        auto* sym = ctx.makeSymbol<FieldSymbol>();
        sym->name = fd->name;
        sym->visibility = fd->visibility;
        sym->modifiers = fd->modifiers;
        sym->parent = &parent;
        sym->decl = fd;
        sym->scope = parent.memberScope;  // field lives in type's member scope
        return sym;
    }
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        auto* sym = ctx.makeSymbol<MethodSymbol>();
        sym->name = md->name;
        sym->visibility = md->visibility;
        sym->modifiers = md->modifiers;
        sym->parent = &parent;
        sym->decl = md;
        sym->scope = scope;
        sym->genericParams =
            collectGenericParams(md->genericParams, ctx, scope);
        sym->params = collectParams(md->params, ctx, scope);
        return sym;
    }
    if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
        auto* sym = ctx.makeSymbol<MethodSymbol>();
        sym->name = od->opToken;
        sym->visibility = od->visibility;
        sym->parent = &parent;
        sym->decl = od;
        sym->scope = scope;
        sym->params = collectParams(od->params, ctx, scope);
        sym->isOperator = true;
        sym->operatorToken = od->opToken;
        return sym;
    }
    if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        auto* sym = ctx.makeSymbol<ConstructorSymbol>();
        // Constructors have no Identifier name in the source; we use the
        // synthetic ".ctor" as the lookup key (mirrors the convention in
        // most CLR-family compilers — sema's name table can disambiguate).
        sym->name = cd->isStatic ? ".cctor" : ".ctor";
        sym->visibility = cd->visibility;
        sym->parent = &parent;
        sym->decl = cd;
        sym->scope = scope;
        sym->isStatic = cd->isStatic;
        sym->params = collectParams(cd->params, ctx, scope);
        return sym;
    }
    if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        auto* sym = ctx.makeSymbol<DestructorSymbol>();
        sym->name = ".dtor";
        sym->parent = &parent;
        sym->decl = dd;
        sym->scope = scope;
        return sym;
    }
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(&m)) {
        auto* sym = ctx.makeSymbol<PropertySymbol>();
        sym->name = pd->isIndexer ? std::string_view(".this") : pd->name;
        sym->visibility = pd->visibility;
        sym->modifiers = pd->modifiers;
        sym->parent = &parent;
        sym->decl = pd;
        sym->scope = scope;
        sym->isIndexer = pd->isIndexer;
        if (pd->isIndexer) {
            sym->indexerParams =
                collectParams(pd->indexerParams, ctx, scope);
        }
        return sym;
    }
    if (auto* ed = dynamic_cast<const ast::EventDecl*>(&m)) {
        auto* sym = ctx.makeSymbol<EventSymbol>();
        sym->name = ed->name;
        sym->visibility = ed->visibility;
        sym->parent = &parent;
        sym->decl = ed;
        sym->scope = parent.memberScope;
        return sym;
    }
    if (auto* cd = dynamic_cast<const ast::ConstDecl*>(&m)) {
        auto* sym = ctx.makeSymbol<ConstMemberSymbol>();
        sym->name = cd->name;
        sym->visibility = cd->visibility;
        sym->parent = &parent;
        sym->decl = cd;
        sym->scope = parent.memberScope;
        sym->isStatic = cd->isStatic;
        return sym;
    }
    if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        auto* wrapper = ctx.makeSymbol<NestedTypeSymbol>();
        wrapper->name = nt->decl ? nt->decl->name : std::string_view();
        wrapper->parent = &parent;
        wrapper->decl = nt;
        wrapper->scope = parent.memberScope;
        if (nt->decl) {
            wrapper->visibility = nt->decl->visibility;
            wrapper->type =
                collectTypeDecl(*nt->decl, ctx, parent.memberScope);
        }
        return wrapper;
    }
    // Unknown Member kind — return null so the caller can skip without
    // diagnosing (parser would already have errored if it built one).
    return nullptr;
}

TypeSymbol* collectTypeDecl(const ast::TypeDecl& td, SemaContext& ctx,
                            ScopeId parentScope) {
    auto* sym = ctx.makeSymbol<TypeSymbol>();
    sym->name = td.name;
    sym->kind = td.kind;
    sym->visibility = td.visibility;
    sym->modifiers = td.modifiers;
    sym->decl = &td;
    sym->scope = parentScope;
    // Each type opens a fresh scope for its members + generic params. Save
    // it on the symbol so nested-member collection can use it as the
    // member's `scope` (a nested type lives inside the outer type's member
    // scope, not in the outer type's enclosing scope).
    sym->memberScope = ctx.newScope();
    sym->genericParams =
        collectGenericParams(td.genericParams, ctx, sym->memberScope);
    for (const auto& m : td.members) {
        if (!m) continue;
        if (auto* member = collectMember(*m, *sym, ctx)) {
            sym->members.push_back(member);
        }
    }
    // §9.1: a record's positional fields become public read-only members,
    // alongside any body-declared members. They carry no body `FieldDecl`, so
    // the FieldSymbol's `decl` is the owning TypeDecl and its type is read
    // from `posParam->type` (resolved by TypeResolution, which already walks
    // td.positionalFields).
    if (td.hasPositionalFields) {
        for (const auto& pf : td.positionalFields) {
            auto* fs = ctx.makeSymbol<FieldSymbol>();
            fs->name = pf.name;
            fs->visibility = ast::Visibility::Public;
            fs->parent = sym;
            fs->decl = &td;
            fs->posParam = &pf;
            fs->scope = sym->memberScope;
            sym->members.push_back(fs);
        }
    }
    // Enum constants live alongside members.
    for (const auto& ec : td.enumConstants) {
        auto* esym = ctx.makeSymbol<EnumConstantSymbol>();
        esym->name = ec.name;
        esym->parent = sym;
        esym->scope = sym->memberScope;
        sym->members.push_back(esym);
    }
    return sym;
}

Symbol* collectTopDecl(const ast::TopDecl& decl, SemaContext& ctx,
                       ScopeId parentScope) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&decl)) {
        return collectTypeDecl(*td, ctx, parentScope);
    }
    if (auto* alias = dynamic_cast<const ast::TypeAlias*>(&decl)) {
        auto* sym = ctx.makeSymbol<TypeAliasSymbol>();
        sym->name = alias->name;
        sym->decl = alias;
        sym->scope = parentScope;
        const ScopeId aliasScope = ctx.newScope();
        sym->genericParams =
            collectGenericParams(alias->genericParams, ctx, aliasScope);
        return sym;
    }
    if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&decl)) {
        auto* sym = ctx.makeSymbol<ExtensionSymbol>();
        // Extension's "name" is empty in the AST; we use an empty name as
        // the symbol key (lookups never find an extension by name —
        // extensions resolve via target type during typecheck).
        sym->decl = ext;
        sym->scope = parentScope;
        const ScopeId extScope = ctx.newScope();
        // We need a dummy TypeSymbol-shape parent for member collection;
        // since extension members aren't owned by a real type at this
        // point (target type isn't resolved yet), we collect them into a
        // private list and patch the parent later. For slice 1, we leave
        // `MemberSymbol::parent` null on extension members.
        TypeSymbol pseudoParent;
        pseudoParent.scope = parentScope;
        pseudoParent.memberScope = extScope;
        for (const auto& m : ext->members) {
            if (!m) continue;
            if (auto* member = collectMember(*m, pseudoParent, ctx)) {
                if (auto* ms = dynamic_cast<MemberSymbol*>(member)) {
                    ms->parent = nullptr; // patched by slice 2
                }
                sym->members.push_back(member);
            }
        }
        return sym;
    }
    if (auto* xb = dynamic_cast<const ast::ExternBlock*>(&decl)) {
        auto* sym = ctx.makeSymbol<ExternBlockSymbol>();
        sym->library = xb->library;
        sym->convention = xb->convention;
        sym->decl = xb;
        sym->scope = parentScope;
        const ScopeId xbScope = ctx.newScope();
        for (const auto& d : xb->methods) {
            auto* dsym = ctx.makeSymbol<ExternDeclSymbol>();
            dsym->name = d.name;
            dsym->visibility = d.visibility;
            dsym->scope = xbScope;
            dsym->params = collectParams(d.params, ctx, xbScope);
            dsym->externDecl = &d;
            sym->decls.push_back(dsym);
        }
        return sym;
    }
    return nullptr;
}

} // namespace

bool collectSymbols(const ast::SourceFile& src, SemaContext& ctx) {
    auto* mod = ctx.makeSymbol<ModuleSymbol>();
    mod->scope = kModuleScope;
    if (src.module) {
        mod->name = moduleSymbolName(src.module->name.get());
        mod->decl = src.module.get();
    } else {
        ctx.diagnostics().report(
            Severity::Error, SourceLocation{},
            "source file is missing a module declaration");
    }
    ctx.setModule(mod);

    for (const auto& topDecl : src.topDecls) {
        if (!topDecl) continue;
        if (auto* child = collectTopDecl(*topDecl, ctx, kModuleScope)) {
            mod->children.push_back(child);
        }
    }
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
