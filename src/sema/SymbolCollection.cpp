#include "sema/SymbolCollection.hpp"

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

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

// §3.4 (#1081): the PascalCase primitive-alias library type names are RESERVED —
// a user type (class/record/enum/interface/type alias) may not be declared with
// one of these names. Each names a compiler-known primitive value type (or
// `string`) through its lowercase keyword alias, and §3.4's dual-form rule makes
// the PascalCase form interchangeable with the keyword everywhere a type is
// named; a user type with one of these names would silently shadow the primitive
// at some use sites and not others (the #1081 wrong-value bug: `record Single`
// resolved to `float`, so `new Single(42).v` read 0). `Object` is intentionally
// NOT reserved (§3.4: it has no keyword alias and is referenced unconditionally),
// and the lowercase keywords cannot reach a type-decl name (the lexer tokenizes
// them as keywords, not identifiers).
bool isReservedPrimitiveAliasName(std::string_view n) {
    return n == "Boolean" || n == "Byte" || n == "SByte" || n == "Int16" ||
           n == "UInt16"  || n == "Int32" || n == "UInt32" || n == "Int64" ||
           n == "UInt64"  || n == "Single" || n == "Float" || n == "Double" ||
           n == "Decimal" || n == "Char"  || n == "String";
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
    // §3.4 (#1081): reject a user type whose name is a reserved PascalCase
    // primitive-alias (Single/Double/Int32/String/…). The symbol is still
    // registered so later passes have a well-formed type to resolve against; the
    // diagnostic makes the program a compile error.
    if (isReservedPrimitiveAliasName(td.name)) {
        ctx.diagnostics().report(
            Severity::Error, td.span.begin,
            std::string("`") + std::string(td.name) +
            "` is a reserved primitive-alias type name (§3.4) and may not be "
            "declared as a user type — it names a built-in primitive value type "
            "through its lowercase keyword alias; choose a different name");
    }
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
    // §9.1 / §9.2 (issue #501): mirror the AST flag onto the TypeSymbol so that
    // imported positional records (which have no ast::TypeDecl in the importer)
    // still pass the §9.2 auto-equality-bearing check via TypeSymbol::hasPositionalFields.
    sym->hasPositionalFields = td.hasPositionalFields;
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
                // §26 (#433): tag an extension METHOD with its declaring
                // block so codegen can recover the extended (receiver) type.
                if (auto* methSym = dynamic_cast<MethodSymbol*>(member)) {
                    methSym->extensionDecl = ext;
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

// §9.2 / §22.9: see Symbol.hpp. An auto-property is a non-static, non-indexer
// property declared in the auto form (`Name: T;` — no expression body, no
// accessor block). Mirrors codegen's isAutoProperty so both layers classify
// storage identically.
bool isAutoPropertySymbol(const Symbol* m) {
    auto* ps = dynamic_cast<const PropertySymbol*>(m);
    if (!ps || ps->isIndexer) return false;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd || pd->isIndexer || pd->bodyKind != ast::PropertyBodyKind::Auto)
        return false;
    for (auto mod : ps->modifiers)
        if (mod == "static") return false;
    return true;
}

// §9.3 (v0.158, #1383): forward declaration for mutual recursion with
// isAdmissibleComponentType — a positional field's type calls
// isAdmissibleComponentType recursively, which in turn calls this function
// to check whether a nested positional record is itself equality-bearing.
// Definition follows isAutoEqualityBearingBodyRecord below.
bool isPositionalRecordEqualityBearing(const TypeSymbol* rec,
                                        const SemaContext& ctx);

// §9.2 v0.158: resolve a record storage component (an auto-property, a body
// field, or a §9.1 positional field) to its declared TypeRef. Returns null when
// the type is not resolvable (an imported member with no local ast::Type node
// and no side-table entry) — the caller then treats the component conservatively
// as admissible.
static const TypeRef* storageComponentType(const Symbol* m,
                                           const SemaContext& ctx) {
    if (auto* fs = dynamic_cast<const FieldSymbol*>(m)) {
        // §9.1 positional field: type rides on the Param.
        if (fs->posParam && fs->posParam->type)
            return ctx.resolvedType(*fs->posParam->type);
        // Body field: the ast::FieldDecl carries the type.
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
            if (fd->type) return ctx.resolvedType(*fd->type);
        return ctx.importedMemberType(m);
    }
    if (auto* ps = dynamic_cast<const PropertySymbol*>(m)) {
        if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl))
            if (pd->type) return ctx.resolvedType(*pd->type);
        return ctx.importedMemberType(m);
    }
    return nullptr;
}

// §9.2 v0.158 admissible-component-type predicate (one recursive rule, applied
// per storage component):
//   - a primitive value type, an enum, or `string`;
//   - another record that is itself auto-equality-bearing (recursively);
//   - `T?` where `T` is admissible (the null-safe `=` of §8.4/§7.11);
//   - a non-owning inline array `[N]T`/`[N]T?` whose element is admissible
//     (element-by-element per §8.3's inline-array rows).
// A class/interface/`Object`/function value/owning-inline-array component is NOT
// admissible (they are reference or move-only types, not structural value
// components). An unresolvable component type (null / Error / an unresolved
// generic parameter) is treated conservatively as admissible so a generic body
// or an imported record is never over-rejected — §3.6 rule 2 re-checks concrete
// generic arguments at each instantiation. `depth` guards the (already
// type-finite) record recursion.
//
// (v0.158 removed the former "sealed reference type with content equality"
// clause; its only instance, `string`, is admitted explicitly above.)
static bool isAdmissibleComponentType(const TypeRef* ty, const SemaContext& ctx,
                                      int depth) {
    using K = TypeRef::Kind;
    if (!ty || depth > 32) return true;  // unresolved / recursion guard: conservative
    // `T?` where T is admissible.
    if (ty->nullable) {
        TypeRef stripped = *ty;
        stripped.nullable = false;
        return isAdmissibleComponentType(&stripped, ctx, depth + 1);
    }
    switch (ty->kind) {
        case K::Error:
            return true;                      // suppress cascade
        case K::Unit:
        case K::Function:
            return false;
        case K::Primitive:
            // A primitive value type or `string` is admissible; `Object` (the
            // root class) is a reference type and is NOT.
            return ty->primitive != PrimitiveType::Object;
        case K::InlineArray:
            // A NON-OWNING inline array `[N]T`/`[N]T?` with an admissible
            // element. A move-only owning inline array (`[N owned]T`, §15.3) is
            // not admissible.
            if (ty->arrayOwned) return false;
            return isAdmissibleComponentType(ty->arrayElement, ctx, depth + 1);
        case K::Named: {
            const TypeSymbol* decl = ty->declaration;
            if (!decl) return true;           // unresolved generic param / imported builtin
            if (decl->kind == ast::TypeDeclKind::Enum) return true;
            if (decl->kind == ast::TypeDeclKind::Record) {
                // Another record — admissible iff it is itself
                // equality-bearing. §9.3 (v0.158, #1383): a positional or
                // combined-form record synthesizes equality only when every
                // positional field (and body member for combined form) is
                // admissible; the blind `return true` for all positional
                // records was the pre-#1383 bug (over-accept). Imported
                // positional records (no ast::TypeDecl) remain conservative.
                if (decl->hasPositionalFields)
                    return isPositionalRecordEqualityBearing(decl, ctx);
                if (auto* td = dynamic_cast<const ast::TypeDecl*>(decl->decl))
                    if (td->hasPositionalFields)
                        return isPositionalRecordEqualityBearing(decl, ctx);
                return isAutoEqualityBearingBodyRecord(decl, ctx);
            }
            // class / interface — reference types are not admissible.
            return false;
        }
    }
    return true;
}

// §9.2: auto-equality eligibility for a body record. The spec's membership
// rule ("A body-only record receives auto-generated `=`, `<>`, `GetHashCode`,
// and `ToString` over its auto-properties … when every member of the record
// is one of: an auto-property; a method or expression-bodied property; an
// overload of a non-equality operator; a constructor") is checked
// structurally. The disqualifying members the spec names that are statically
// decidable here are: an explicit field declaration (storage that is not an
// auto-property), and a manually-declared equality member (per §8.5 the auto
// package is suppressed when `Equals`/`GetHashCode`/`ToString` is hand-written,
// and per the §9.2 "Authors who want explicit equality … declare `=` and `<>`"
// clause a user `=`/`<>` overload opts out).
//
// The §9.2 "non-trivial constructor body" disqualifier IS now decided here
// (#770): a constructor whose body does work beyond property initialization
// (control flow, locals, throws, non-property-target assignments) opts the
// record out of auto-equality. The "side-effecting computed property"
// disqualifier remains out of scope; computed (expression-bodied / block)
// properties are treated as non-disqualifying (the spec's stated default for
// the trivial forms).
bool isAutoEqualityBearingBodyRecord(const TypeSymbol* rec,
                                     const SemaContext& ctx) {
    if (!rec || rec->kind != ast::TypeDeclKind::Record) return false;
    // §9.3: a record with positional fields is positional / combined form; it
    // gets the §9.1 positional synthesis, not this body-record path.
    if (rec->hasPositionalFields) return false;
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(rec->decl))
        if (td->hasPositionalFields) return false;

    for (const Symbol* m : rec->members) {
        // An auto-property is the participating storage — eligible only when its
        // declared type is an admissible component type (§9.2 v0.158).
        if (isAutoPropertySymbol(m)) {
            if (!isAdmissibleComponentType(storageComponentType(m, ctx), ctx, 0))
                return false;
            continue;
        }
        // A non-auto property (expression-bodied or block-form) is a computed
        // member: it does not contribute storage and does not disqualify.
        if (dynamic_cast<const PropertySymbol*>(m)) continue;
        // An explicit field declaration is storage that is not an
        // auto-property → disqualifies (§9.2).
        if (dynamic_cast<const FieldSymbol*>(m)) return false;
        if (auto* meth = dynamic_cast<const MethodSymbol*>(m)) {
            // A manually-declared equality member suppresses the auto package
            // (§9.2): an `Equals`/`GetHashCode` method, or an `=`/`<>` operator
            // overload.
            if (meth->isOperator) {
                if (meth->operatorToken == "=" || meth->operatorToken == "<>")
                    return false;
                continue;  // any other operator overload is fine (§9.2)
            }
            // §9.2 v0.158 (narrowed, was #1249-A): a `ToString` override alone no
            // longer disqualifies — it suppresses only auto-`ToString` (the
            // author took over formatting, not equality). Auto-`Equals`/
            // `GetHashCode`/`=` remain; `.ToString()` dispatches to the override
            // (codegen prefers the user method). Only `Equals`/`GetHashCode`
            // (the equality surface) still disqualify.
            if (meth->name == "Equals" || meth->name == "GetHashCode")
                return false;
            continue;  // ordinary methods (and `ToString`) do not disqualify (§9.2)
        }
        // §9.2 (#770): a constructor disqualifies auto-equality when its body
        // "performs work other than property initialization." A TRIVIAL body is
        // one whose every statement is a property-initialization assignment
        // (`P := e` / `this.P := e`, op `:=`, target a bare name or `this.`
        // member). Any other statement form — control flow (`if`/`while`/`for`/
        // `case`/`try`), a `throw`, a local declaration, a compound or non-
        // property-target assignment — is "other work" and opts the record out
        // of auto-equality (the author must then declare explicit `=`/`<>`). A
        // constructor-chain ExpressionStmt is conservatively treated as trivial
        // (a residual leniency for a side-effecting bare call — far narrower
        // than the prior blanket acceptance of every constructor body).
        if (auto* cs = dynamic_cast<const ConstructorSymbol*>(m)) {
            if (auto* cd =
                    dynamic_cast<const ast::ConstructorDecl*>(cs->decl)) {
                for (const auto& st : cd->body) {
                    if (!st) continue;
                    if (auto* as =
                            dynamic_cast<const ast::AssignmentStmt*>(st.get())) {
                        if (as->op == ":=" && as->target &&
                            (dynamic_cast<const ast::IdentifierExpr*>(
                                 as->target.get()) ||
                             dynamic_cast<const ast::MemberAccessExpr*>(
                                 as->target.get())))
                            continue;  // property-initialization — trivial
                        return false;  // compound / other-target — non-trivial
                    }
                    if (dynamic_cast<const ast::ExpressionStmt*>(st.get()))
                        continue;  // constructor-chain call — conservatively ok
                    return false;  // any other statement kind — non-trivial
                }
            }
            continue;
        }
        // Const members and nested types carry no instance storage and do not
        // participate in equality; they are benign.
        if (dynamic_cast<const ConstMemberSymbol*>(m)) continue;
        if (dynamic_cast<const NestedTypeSymbol*>(m)) continue;
        // Anything else (e.g. an event, a destructor — neither is valid on a
        // record per §9.7) is conservatively treated as disqualifying.
        return false;
    }
    return true;
}

// §9.3 (v0.158, #1383): true when a positional or combined-form record
// synthesizes equality. Every positional field's type must be an admissible
// component type (§9.2 recursive rule). For the combined form, body members must
// not include disqualifiers (§9.3 applies §9.2's membership and disqualifier
// rules to the full storage set: positional fields + body auto-properties).
// Imported positional records (no ast::TypeDecl — only hasPositionalFields flag)
// are conservatively treated as equality-bearing: individual field types are not
// available across module boundaries (see the §3.6 rule-2 per-instantiation
// discipline; this function is the LOCAL record side, not the cross-module side).
bool isPositionalRecordEqualityBearing(const TypeSymbol* rec,
                                        const SemaContext& ctx) {
    if (!rec) return false;
    // Imported positional record: no ast::TypeDecl to inspect → conservative.
    if (!dynamic_cast<const ast::TypeDecl*>(rec->decl)) return true;
    for (const Symbol* m : rec->members) {
        if (auto* fs = dynamic_cast<const FieldSymbol*>(m)) {
            if (fs->posParam) {
                // Positional field: type must be admissible (§9.3 → §9.2 rule).
                if (!isAdmissibleComponentType(
                        storageComponentType(m, ctx), ctx, 0))
                    return false;
            } else {
                // Explicit body field declaration: disqualifying per §9.2.
                return false;
            }
            continue;
        }
        // Body auto-property of combined form: type must be admissible.
        if (isAutoPropertySymbol(m)) {
            if (!isAdmissibleComponentType(storageComponentType(m, ctx), ctx, 0))
                return false;
            continue;
        }
        // Computed (expression-bodied) property: no storage, does not disqualify.
        if (dynamic_cast<const PropertySymbol*>(m)) continue;
        if (auto* meth = dynamic_cast<const MethodSymbol*>(m)) {
            if (meth->isOperator) {
                // User `=`/`<>` operator: opts the record out of auto-equality.
                if (meth->operatorToken == "=" || meth->operatorToken == "<>")
                    return false;
                continue;  // other operator overloads are benign
            }
            // Equals/GetHashCode override: disqualifying (§9.2 v0.158).
            if (meth->name == "Equals" || meth->name == "GetHashCode")
                return false;
            continue;  // ordinary methods and ToString override are benign
        }
        if (auto* cs = dynamic_cast<const ConstructorSymbol*>(m)) {
            // Same trivial-body check as body records (§9.3 → §9.2, #770).
            if (auto* cd =
                    dynamic_cast<const ast::ConstructorDecl*>(cs->decl)) {
                for (const auto& st : cd->body) {
                    if (!st) continue;
                    if (auto* as =
                            dynamic_cast<const ast::AssignmentStmt*>(
                                st.get())) {
                        if (as->op == ":=" && as->target &&
                            (dynamic_cast<const ast::IdentifierExpr*>(
                                 as->target.get()) ||
                             dynamic_cast<const ast::MemberAccessExpr*>(
                                 as->target.get())))
                            continue;
                        return false;
                    }
                    if (dynamic_cast<const ast::ExpressionStmt*>(st.get()))
                        continue;
                    return false;
                }
            }
            continue;
        }
        if (dynamic_cast<const ConstMemberSymbol*>(m)) continue;
        if (dynamic_cast<const NestedTypeSymbol*>(m)) continue;
        return false;
    }
    return true;
}

// §9.2 (v0.156)/§8.4: returns true only when the record has declared an
// explicit `operator =` or `operator <>` overload.  Per §9.2 v0.156 the
// complete form is: "Authors who want explicit equality on a record outside the
// auto-equality-bearing set declare `=` and `<>` overloads in the body, along
// with `Equals(Object?)`, `Equals(T)`, and `GetHashCode` overrides as needed."
// Only the operator overloads bypass the §9.2 compile error on `=`/`<>`: a
// record that declares `override Equals`/`override GetHashCode` WITHOUT the
// operators is NOT auto-equality-bearing (those overrides are themselves
// disqualifiers per isAutoEqualityBearingBodyRecord above), and `=`/`<>` on
// it remains a compile error (#1249).  The `.Equals`/`.GetHashCode` call path
// is separately gated: lookupMember finds the user method and dispatches
// directly, so that path does not reach this function.
bool recordHasUserDeclaredEquality(const TypeSymbol* rec) {
    if (!rec) return false;
    for (const Symbol* m : rec->members) {
        if (auto* meth = dynamic_cast<const MethodSymbol*>(m)) {
            if (meth->isOperator &&
                (meth->operatorToken == "=" || meth->operatorToken == "<>"))
                return true;
        }
    }
    return false;
}

// §3.6 / §8.3 (v0.158, #1338): see Symbol.hpp. A type is equality-bearing unless
// it is a record that neither synthesizes equality nor declares user equality.
bool isEqualityBearing(const TypeRef* ty, const SemaContext& ctx) {
    if (!ty) return true;                              // unresolved: conservative
    // The underlying type carries the equality contract; a nullable key `K?`
    // is equality-bearing iff `K` is (the null-safe `=` of §8.4 handles null).
    if (ty->kind != TypeRef::Kind::Named || !ty->declaration)
        return true;  // primitive / string / unit / inline array / function /
                      // unresolved (generic param, imported builtin) — all bear
                      // consistent equality.
    const TypeSymbol* ts = ty->declaration;
    if (ts->kind != ast::TypeDeclKind::Record)
        return true;  // class (reference identity), interface, enum — all bear
                      // consistent Equals/GetHashCode.
    // A record: equality-bearing iff it synthesizes equality (positional /
    // combined §9.1/§9.3, or an auto-equality-bearing body record §9.2) or
    // declares user equality (an `operator =`/`<>` overload — its §25.2
    // co-declared `Equals(T)`/`GetHashCode` companions are separately enforced).
    // §9.3 (v0.158, #1383): positional/combined records must pass the
    // admissibility check; the blind `return true` was the pre-#1383 bug.
    if (ts->hasPositionalFields)
        return isPositionalRecordEqualityBearing(ts, ctx) ||
               recordHasUserDeclaredEquality(ts);
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
        if (td->hasPositionalFields)
            return isPositionalRecordEqualityBearing(ts, ctx) ||
                   recordHasUserDeclaredEquality(ts);
    return isAutoEqualityBearingBodyRecord(ts, ctx) ||
           recordHasUserDeclaredEquality(ts);
}

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
