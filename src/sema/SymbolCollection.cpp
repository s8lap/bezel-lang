#include "sema/SymbolCollection.hpp"

#include "ast/Node.hpp"
#include "common/BuiltinRegistry.hpp"  // isBuiltinExceptionName — §31.4 (#2950)
#include "common/OwnershipFacts.hpp"  // classifyChoiceType — §35.7 equality-bearing choice edge (#2083/#2087)
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

// #1488 / §2.1 / §2.8.3 rule 3: the module's FULL dotted name
// ("BankingApp.Transactions"), as opposed to moduleSymbolName's leaf-only
// symbol name above. This is the module-identity component every TypeSymbol
// carries in TypeSymbol::moduleName, so codegen can qualify emitted symbol
// names and two modules' same-named public types never collide at link
// time. Mirrors ModuleMetadataBuilder::moduleNameFor's identical
// reconstruction (needed there for the exported interface's module
// identity) — duplicated in miniature here rather than shared, since
// SymbolCollection (sema/) runs before and has no dependency on the
// module/ layer.
std::string moduleDottedName(const ModuleSymbol* mod) {
    if (!mod) return {};
    if (auto* decl = dynamic_cast<const ast::ModuleDecl*>(mod->decl)) {
        if (decl->name && !decl->name->parts.empty()) {
            std::string out;
            for (std::size_t i = 0; i < decl->name->parts.size(); ++i) {
                if (i) out += '.';
                out += std::string(decl->name->parts[i]);
            }
            return out;
        }
    }
    return std::string(mod->name);
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
        // §22.4 (v0.183, #1585): carry the out-nullability postcondition onto
        // the symbol so narrowing and override-match can consult it.
        sym->outNullPostcond = p.outNullPostcond;
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
        // fix #2418: mirror the TypeSymbol precedent (SymbolCollection.cpp,
        // collectTypeDecl) — every ast::Member subtype inherits `span` from
        // ast::Node, so its own decl node's span is a real, available
        // location. Without this, every diagnostic keyed on
        // FieldSymbol::declLoc reported 0:0.
        sym->declLoc = fd->span.begin;
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
        sym->declLoc = md->span.begin;  // fix #2418
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
        sym->declLoc = od->span.begin;  // fix #2418
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
        sym->declLoc = cd->span.begin;  // fix #2418
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
        sym->declLoc = dd->span.begin;  // fix #2418
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
        sym->declLoc = pd->span.begin;  // fix #2418
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
        sym->declLoc = ed->span.begin;  // fix #2418
        sym->scope = parent.memberScope;
        return sym;
    }
    if (auto* cd = dynamic_cast<const ast::ConstDecl*>(&m)) {
        auto* sym = ctx.makeSymbol<ConstMemberSymbol>();
        sym->name = cd->name;
        sym->visibility = cd->visibility;
        sym->parent = &parent;
        sym->decl = cd;
        sym->declLoc = cd->span.begin;  // fix #2418
        sym->scope = parent.memberScope;
        sym->isStatic = cd->isStatic;
        return sym;
    }
    if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        auto* wrapper = ctx.makeSymbol<NestedTypeSymbol>();
        wrapper->name = nt->decl ? nt->decl->name : std::string_view();
        wrapper->parent = &parent;
        wrapper->decl = nt;
        wrapper->declLoc = nt->span.begin;  // fix #2418
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
    // #1488: the declaring (local, current-compilation) module's full dotted
    // name — see TypeSymbol::moduleName. collectTypeDecl runs for both
    // top-level and nested type declarations, and ctx.module() is already set
    // by collectSymbols before either kind is collected.
    sym->moduleName = moduleDottedName(ctx.module());
    // fix #1761: copy the AST declaration's source location onto the symbol
    // so diagnostics keyed on t->declLoc / m->declLoc point at the actual
    // declaration site rather than reporting 0:0. The parser now stamps
    // `td.span.begin` for top-level type decls (see Parser_Decls.cpp's
    // parseTypeOrInterfaceDecl); for other node types that already had their
    // span set this propagates that location to the symbol.
    sym->declLoc = td.span.begin;
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
            // fix #2482 (follow-up to #2418): ast::Param now carries its own
            // source location (stamped by the parser at the field's first
            // token), so point at the member's own line rather than the
            // enclosing type's. The type-fallback remains a safety net for a
            // decl whose loc was not populated (default 0:0:0 — e.g. a
            // synthesized prelude decl built without going through the parser).
            fs->declLoc = (pf.loc.line != 0) ? pf.loc : sym->declLoc;
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
        // fix #2482 (follow-up to #2418): ast::EnumConstant now carries its
        // own source location (stamped at the constant's name token); fall
        // back to the enclosing TypeDecl's location only when unpopulated.
        esym->declLoc = (ec.loc.line != 0) ? ec.loc : sym->declLoc;
        esym->scope = sym->memberScope;
        sym->members.push_back(esym);
    }
    // §35 (v0.200, #2044): choice cases live alongside members, like enum
    // constants. Each case carries a pointer back to its AST so downstream
    // passes can read its payload fields (§35.4 classification, §35.5
    // matching, §35.3 construction).
    for (const auto& cc : td.choiceCases) {
        auto* csym = ctx.makeSymbol<ChoiceCaseSymbol>();
        csym->name = cc.name;
        csym->parent = sym;
        csym->caseDecl = &cc;
        // fix #2482 (follow-up to #2418): ast::ChoiceCase now carries its
        // own source location (stamped at the case's name token); fall back
        // to the enclosing TypeDecl's location only when unpopulated.
        csym->declLoc = (cc.loc.line != 0) ? cc.loc : sym->declLoc;
        csym->scope = sym->memberScope;
        sym->members.push_back(csym);
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
        sym->declLoc = alias->span.begin;  // fix #2418: TypeAlias : TopDecl : Node
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
        sym->declLoc = ext->span.begin;  // fix #2418: ExtensionDecl : TopDecl : Node
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
        sym->declLoc = xb->span.begin;  // fix #2418: ExternBlock : TopDecl : Node
        sym->scope = parentScope;
        const ScopeId xbScope = ctx.newScope();
        for (const auto& d : xb->methods) {
            auto* dsym = ctx.makeSymbol<ExternDeclSymbol>();
            dsym->name = d.name;
            dsym->visibility = d.visibility;
            dsym->scope = xbScope;
            dsym->params = collectParams(d.params, ctx, xbScope);
            dsym->externDecl = &d;
            // fix #2482 (follow-up to #2418): ast::ExternDecl now carries
            // its own source location (stamped at the decl's first token);
            // fall back to the enclosing ExternBlock's location only when
            // unpopulated.
            dsym->declLoc = (d.loc.line != 0) ? d.loc : sym->declLoc;
            sym->decls.push_back(dsym);
        }
        return sym;
    }
    return nullptr;
}

} // namespace

// §22.3 / §2.5 (v0.237, #2659, #2374): see Symbol.hpp for the contract and for
// why every constructor-formal consumer must read BOTH sources. The local
// source is the `ast::ConstructorDecl`'s `params`; the imported source is the
// reconstructed `ParameterSymbol` list, which is all an imported constructor
// has (its `decl` is null).
namespace {
const ast::Param* ctorDeclParam(const ConstructorSymbol& cs, std::size_t pos) {
    auto* d = dynamic_cast<const ast::ConstructorDecl*>(cs.decl);
    if (!d || pos >= d->params.size()) return nullptr;
    return &d->params[pos];
}

const ParameterSymbol* ctorSymParam(const ConstructorSymbol& cs,
                                    std::size_t pos) {
    if (pos >= cs.params.size()) return nullptr;
    return dynamic_cast<const ParameterSymbol*>(cs.params[pos]);
}
} // namespace

std::string_view formalParamModifier(const ConstructorSymbol& cs,
                                     std::size_t pos) {
    if (const ast::Param* p = ctorDeclParam(cs, pos)) return p->modifier;
    if (const ParameterSymbol* p = ctorSymParam(cs, pos)) return p->modifier;
    return {};
}

std::string_view formalParamName(const ConstructorSymbol& cs,
                                 std::size_t pos) {
    if (const ast::Param* p = ctorDeclParam(cs, pos)) return p->name;
    if (const ParameterSymbol* p = ctorSymParam(cs, pos)) return p->name;
    return {};
}

std::size_t formalParamCount(const ConstructorSymbol& cs) {
    if (auto* d = dynamic_cast<const ast::ConstructorDecl*>(cs.decl))
        return d->params.size();
    return cs.params.size();
}

// §12 / §31.4 (#595 / #2950): see Symbol.hpp for the contract. Two sources,
// exactly like the constructor-formal helpers above: the LOCAL source is the
// `ast::TypeDecl`'s base-type list; the IMPORTED source is
// `TypeSymbol::builtinExceptionBase`, which ModuleLoader restores from the
// artifact and which Type Resolution also populates for local types.
//
// The symbol field is consulted FIRST at every level so the two sources can
// never disagree about a class both of them describe, and so a chain that
// alternates local and imported ancestors resolves at whichever level actually
// names the built-in.
//
// Membership is `common::isBuiltinExceptionName` — the single
// `kBuiltinExceptionHierarchy` table — rather than a hand-listed set. The
// hand-listed copies this replaces had already drifted: the §12 `Message`
// member check in ExprTypecheck_Calls.cpp omitted DisposedAccessException,
// SerializationException, ChannelClosedException, ChannelFullException and
// DomainFaultedException, all five of which codegen's `builtinVtable_` gate DID
// carry — so a class deriving from one of them was laid out WITH the header
// while sema rejected `e.Message` on it.
std::string_view builtinExceptionBaseOf(const TypeSymbol* t) {
    if (!t || t->kind != ast::TypeDeclKind::Class) return {};
    // Bound the walk. Sema rejects an inheritance cycle before codegen runs, so
    // this can only be reached with an acyclic chain — but the walk no longer
    // requires an `ast::TypeDecl` at each level (an imported ancestor has
    // none), so it must not be able to spin on a malformed symbol graph either.
    unsigned guard = 0;
    for (const TypeSymbol* cur = t; cur != nullptr && guard < 256u; ++guard) {
        if (!cur->builtinExceptionBase.empty()) return cur->builtinExceptionBase;
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(cur->decl)) {
            for (const auto& bt : td->baseTypes) {
                if (!bt || !bt->name || bt->name->parts.empty()) continue;
                std::string_view n = bt->name->parts.back();
                if (common::isBuiltinExceptionName(n)) return n;
            }
        }
        // Advance to the next CLASS ancestor. #2950: an imported ancestor has
        // `decl == nullptr`, and the pre-#2950 walks required `b->decl` here —
        // which stopped the walk dead at the module boundary, so a LOCAL class
        // deriving from an IMPORTED exception subclass lost the header too
        // (measured: `%class.Local = type { ptr, i8, i32, i32 }` against the
        // exporter's 52-byte `ExA` chain, and its inherited field read 0).
        const TypeSymbol* next = nullptr;
        for (const TypeSymbol* b : cur->baseTypes) {
            if (b && b != cur && b->kind == ast::TypeDeclKind::Class) {
                next = b;
                break;
            }
        }
        cur = next;
    }
    return {};
}

// §31.4 (v0.240, #2971/#2983/#2984): see Symbol.hpp for the contract and for
// what is deliberately excluded. The ONLY place the §31.4 surface is stated.
//
// It is stated as "which built-in DECLARES this name", then walked up the
// hierarchy — not as "which built-in type names carry this member". The second
// form is the positive-list shape this project's own defect taxonomy names: the
// pre-existing hand-listed `ParamName` receiver set
// ("ArgumentException" || "ArgumentNullException" || "ArgumentOutOfRangeException")
// happens to be complete today only because nobody has added a fourth
// ArgumentException descendant to `kBuiltinExceptionHierarchy`. The walk cannot
// drift: adding an edge to that table is enough.
BuiltinExcMember builtinExceptionSurfaceMemberOf(std::string_view builtinBase,
                                                 std::string_view member,
                                                 std::string_view* owner) {
    if (builtinBase.empty() || member.empty()) return BuiltinExcMember::None;
    // Bound the walk exactly as builtinExceptionBaseOf does: the table is
    // acyclic by construction, but a malformed edit must not spin.
    unsigned guard = 0;
    for (std::string_view cur = builtinBase;
         !cur.empty() && guard < 64u; ++guard) {
        auto found = [&](BuiltinExcMember k) {
            if (owner) *owner = cur;
            return k;
        };
        // §31.4 `public class ArgumentException: Exception … ParamName: string?`
        if (cur == "ArgumentException" && member == "ParamName")
            return found(BuiltinExcMember::ParamName);
        // §31.4 `public class Exception … Message / StackTrace / CauseChain`
        if (cur == "Exception") {
            if (member == "Message")    return found(BuiltinExcMember::Message);
            if (member == "StackTrace") return found(BuiltinExcMember::StackTrace);
            if (member == "CauseChain") return found(BuiltinExcMember::CauseChain);
        }
        cur = common::builtinExceptionParentOf(cur);
    }
    return BuiltinExcMember::None;
}

BuiltinExcMember builtinExceptionSurfaceMember(const TypeSymbol* t,
                                               std::string_view member) {
    return builtinExceptionSurfaceMemberOf(builtinExceptionBaseOf(t), member);
}

std::string builtinExceptionSurfaceList(std::string_view builtinBase) {
    // Built by ASKING the resolver, member by member, rather than by a second
    // hand-written list: a diagnostic that names a surface the resolver would
    // reject is the #2983 defect over again (a message whose factual claim is
    // false), so the two must share one source of truth.
    static constexpr std::string_view kNames[] = {"Message", "StackTrace",
                                                  "CauseChain", "ParamName"};
    std::string out;
    for (std::string_view n : kNames) {
        if (builtinExceptionSurfaceMemberOf(builtinBase, n) ==
            BuiltinExcMember::None)
            continue;
        if (!out.empty()) out += ", ";
        out += std::string(n);
    }
    return out;
}

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

// §35.7 (v0.210, #2083): declared in Symbol.hpp (with the #2047 `inst`
// instantiation parameter). Body is below — after the admissibility predicate
// it depends on (`isAdmissibleComponentType` calls it).

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
//     (element-by-element per §8.3's inline-array rows);
//   - a **value-like choice** that is itself auto-equality-bearing
//     (§35.7, v0.210 #2083) — recursively applying this same list to every
//     case's payload fields.
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
            // §9.2 v0.210 (#2083): a CHOICE component is admissible iff it is
            // value-like AND itself auto-equality-bearing (§35.7). Borrow-bearing
            // and owning choices are NOT admissible in any component — their
            // class-typed payload slots are reference or move-only types, exactly
            // the categories the rule rejects. Auto-equality-bearing for choices
            // is the same predicate used by isEqualityBearing as a hash-key
            // check (and codegen uses it to dispatch the choice-specific
            // Equals/GetHashCode synthesis) — see isAutoEqualityBearingChoice.
            if (decl->kind == ast::TypeDeclKind::Choice)
                return isAutoEqualityBearingChoice(decl, ctx);
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
// v0.182 (#1572): the former #770 "non-trivial constructor body" disqualifier
// and the "side-effecting computed property" disqualifier are BOTH retired —
// equality/hashing/ToString are defined over storage (auto-properties) only,
// so no constructor-body or computed-property-body behavior can affect their
// correctness (a validating or normalizing constructor, e.g. `Width > 0 and
// Height > 0`, no longer opts a record out of `=`). A constructor — instance
// or static, regardless of what its body does — is therefore unconditionally
// non-disqualifying. Computed (expression-bodied / block) properties were
// already treated as non-disqualifying regardless of body; that is unchanged.
//
// v0.182 also codifies (§9.2 "Static members are ignored"): `const`
// declarations, `static readonly` fields, and static methods/properties
// contribute no instance storage and neither qualify nor disqualify. Static
// auto-properties and static methods already fell through as benign (the
// PropertySymbol/MethodSymbol arms below do not distinguish static); a
// `static readonly` FIELD (an explicit FieldDecl carrying the `static`
// modifier — the FieldSymbol arm) did NOT get this exemption before v0.182
// and was wrongly disqualifying a record for exactly the "Origin-style
// constant" pattern §9.2 gives as the motivating example — fixed here.
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
        if (auto* fs = dynamic_cast<const FieldSymbol*>(m)) {
            // §9.2 v0.182: a `static` field (e.g. `static readonly`) carries no
            // instance storage and is ignored. A non-static explicit field IS
            // storage that is not an auto-property → still disqualifies.
            bool isStatic = false;
            for (auto mod : fs->modifiers)
                if (mod == "static") { isStatic = true; break; }
            if (isStatic) continue;
            return false;
        }
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
        // §9.2 v0.182 (#1572): a constructor — instance or static, regardless
        // of what its body does — never disqualifies auto-equality (the
        // former #770 trivial-body check is retired; see the function
        // comment above).
        if (dynamic_cast<const ConstructorSymbol*>(m)) continue;
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
// are conservatively treated as equality-bearing for the positional-field
// admissibility check (individual field types are not fully inspectable cross-
// module; see the §3.6 rule-2 per-instantiation discipline). For the
// body-member disqualifier scan, an imported record's FieldSymbol IS marked as
// positional vs body via the importer setting `posParam` from the metadata's
// `isPositional` bit (#2516), so the SAME member-walk the local path does
// below applies verbatim — including the body-field disqualifier (#2516's
// finding: a pre-fix unconditional `return true` for the import case
// false-accepted `=`/`<>` on imported combined-form records whose exporter
// already rejects them).
bool isPositionalRecordEqualityBearing(const TypeSymbol* rec,
                                        const SemaContext& ctx) {
    if (!rec) return false;
    for (const Symbol* m : rec->members) {
        if (auto* fs = dynamic_cast<const FieldSymbol*>(m)) {
            if (fs->posParam) {
                // Positional field: type must be admissible (§9.3 → §9.2 rule).
                if (!isAdmissibleComponentType(
                        storageComponentType(m, ctx), ctx, 0))
                    return false;
            } else {
                // §9.2 v0.182 (#1572): a `static` body field (e.g. `static
                // readonly`) carries no instance storage and is ignored —
                // same static-member exemption as the body-record path
                // (isAutoEqualityBearingBodyRecord above). A non-static
                // explicit body field declaration remains disqualifying.
                bool isStatic = false;
                for (auto mod : fs->modifiers)
                    if (mod == "static") { isStatic = true; break; }
                if (!isStatic) return false;
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
        // §9.2 v0.182 (#1572, via §9.3): a constructor — instance or static,
        // regardless of what its body does — never disqualifies auto-equality
        // (the former #770 trivial-body check is retired; see
        // isAutoEqualityBearingBodyRecord's function comment above).
        if (dynamic_cast<const ConstructorSymbol*>(m)) continue;
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

// §35.7 (v0.200, #2044) / v0.210 (#2083): a choice is auto-equality-bearing
// iff (a) its §35.4 classification is ValueLike AND (b) every payload field of
// every case is a §9.2 admissible component type — with "another auto-
// equality-bearing choice" admitted to the recursive rule. Per §35.7's first
// paragraph this is the property that allows `=`, `<>`, `.Equals()`, and
// `.GetHashCode()` on the choice without the §9.2 use-site error. The
// classification checkup-front rejects borrow-bearing and owning choices
// (class-typed payloads are reference/move-only types, outside the admissible
// rule by the same token class-typed record fields are). The per-payload
// admissibility check then covers value payloads that themselves fail the
// §9.2 list (e.g. a payload typed as a class — would be admitted only by
// the existing choice type rejection above, but a value payload of an
// interface, function, or owning-inline-array type would reach here).
//
// Imported choices (no ast::TypeDecl — only the body symbol) are conservatively
// treated as auto-equality-bearing: classifyChoiceType returns nullopt without
// a local AST, and nullopt maps to ACCEPT here (never over-reject a type we
// cannot inspect — the record posture, and #2087's convention). The per-payload
// rule operates on the AST's Param types, which are not locally available
// across module boundaries for an imported choice. The §3.6 rule-2
// per-instantiation discipline re-checks concrete generic arguments at each
// instantiation.
//
// NB: the "non-trivial constructor / method body" §9.2 membership exemption
// does not carry over to choices — choices have no user constructors or
// instance fields, so all equality-relevant storage IS the case payload fields.
// Methods/computed properties declared in the choice body (§35.2) are by
// §35.7 ruled like §9.2: they do not disqualify (a `=`/`<>` overload, an
// `Equals`/`GetHashCode` override, would; but the choice surface is so
// constrained that this is a non-issue in practice — flagged in case a future
// admission widens it). The conservative mirror of the §9.2 record rule
// dispatches on the body member list, but the result is identical for a
// legal choice (every body member is benign).
bool isAutoEqualityBearingChoice(const TypeSymbol* ch,
                                 const SemaContext& ctx,
                                 const TypeRef* inst) {
    if (!ch || ch->kind != ast::TypeDeclKind::Choice) return false;
    // §35.4: compute the classification. classifyChoiceType returns nullopt
    // precisely when it cannot see an ast::TypeDecl — for a choice-kinded
    // symbol that means an IMPORTED choice (no local AST). Treat nullopt as
    // conservative-ACCEPT, matching the record posture (isEqualityBearing's
    // null/unresolved `return true`) and the #2087 convention: a constraint /
    // key check never over-rejects on a type it cannot inspect. (The pre-
    // rework code returned false here, which made its own imported→accept
    // branch below dead code — the review's finding 3.)
    // #2047: classify at the INSTANTIATION when available so the §28.1 erasure
    // of the `owned` markers applies (`Result<int, string>` is value-like).
    TypeRef bare;
    const TypeRef* rt = inst;
    if (!rt || rt->kind != TypeRef::Kind::Named || rt->declaration != ch) {
        bare.kind = TypeRef::Kind::Named;
        bare.declaration = const_cast<TypeSymbol*>(ch);
        rt = &bare;
    }
    auto verdict = common::classifyChoiceType(rt, ctx);
    if (!verdict) return true;
    if (*verdict != common::ChoiceClassification::ValueLike)
        return false;
    // Defensive: classification succeeded, so the AST is present; if it were
    // somehow missing we cannot walk payloads — accept on the ValueLike
    // classification verdict alone (same conservative posture as above).
    auto* td = dynamic_cast<const ast::TypeDecl*>(ch->decl);
    if (!td) return true;
    // Every case: every payload field, at THIS instantiation, must be a §9.2
    // admissible component type. #2047 / §28.1: an `owned` marker on a value-
    // like/runtime-managed payload (the only case that reaches here — the
    // classification gate above already rejected an owning instantiation) has
    // ERASED, so the payload's admissibility is decided by its instantiated
    // type like any component (a `string`/`int` payload is admissible). The
    // recursive `isAdmissibleComponentType` drives the body-record rule and
    // admits "another auto-equality-bearing choice" via its K::Named/Choice
    // branch (v0.210, #2083).
    for (const auto& cc : td->choiceCases) {
        for (const auto& pf : cc.payload) {
            if (!pf.type) continue;  // unresolved → skip (defensive)
            const TypeRef* ft =
                common::substitutedChoicePayloadType(pf, td, rt, ctx);
            if (!isAdmissibleComponentType(ft, ctx, 0))
                return false;
        }
    }
    return true;
}

// §3.6 / §8.3 (v0.158, #1338): see Symbol.hpp. A type is equality-bearing unless
// it is a record that neither synthesizes equality nor declares user equality,
// or a choice that is not value-like auto-equality-bearing (§35.7, v0.214).
bool isEqualityBearing(const TypeRef* ty, const SemaContext& ctx) {
    if (!ty) return true;                              // unresolved: conservative
    // The underlying type carries the equality contract; a nullable key `K?`
    // is equality-bearing iff `K` is (the null-safe `=` of §8.4 handles null).
    if (ty->kind != TypeRef::Kind::Named || !ty->declaration)
        return true;  // primitive / string / unit / inline array / function /
                      // unresolved (generic param, imported builtin) — all bear
                      // consistent equality.
    const TypeSymbol* ts = ty->declaration;
    if (ts->kind == ast::TypeDeclKind::Record) {
        // A record: equality-bearing iff it synthesizes equality (positional /
        // combined §9.1/§9.3, or an auto-equality-bearing body record §9.2) or
        // declares user equality (an `operator =`/`<>` overload — its §25.2
        // co-declared `Equals(T)`/`GetHashCode` companions are separately
        // enforced).
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
    // §35.7 (v0.210/v0.214, #2083/#2087): a CHOICE is equality-bearing exactly
    // when it is auto-equality-bearing — i.e. value-like with all case payloads
    // admissibly typed (the RECURSIVE §35.7 rule: "this same list applied to
    // every case's payload fields"; classification alone under-enforces — a
    // value-like choice with a function/Object payload is value-like but not
    // equality-bearing, the key-position half of #2110). A borrow-bearing or
    // owning choice carries no value equality contract and is NOT
    // equality-bearing (Dictionary<Outcome, _> keys fail here per §3.6). This
    // one computation also decides the §14.2 `where T: hashable` constraint
    // (#2087) — "the constraint adds no second definition of the property" —
    // and the §3.6 rule-2 hash-container-key check, so the constraint, the
    // key check, and the use-site `=`/`<>` error can never disagree. A
    // primitive, string, class, interface, function, enum, or inline array is
    // equality-bearing (the `return true` fall-through applies).
    if (ts->kind == ast::TypeDeclKind::Choice)
        // #2047: pass `ty` (the instantiation, with genericArgs) so a generic
        // choice is judged at its instantiation (`Result<int, string>` is
        // auto-equality-bearing; its `owned` markers erase per §28.1).
        return isAutoEqualityBearingChoice(ts, ctx, ty);
    return true;  // class (reference identity), interface, enum — all bear
                  // consistent Equals/GetHashCode.
}

// §14.2 (v0.214, #2087): the component-path fragment for a FAILED
// `where T: hashable` constraint — see Symbol.hpp. Precondition:
// isEqualityBearing(ty, ctx) == false. The equality-bearing property surfaces
// its failure at the top-level type (a non-auto-equality-bearing record, or a
// non-value-like choice), so the "component path" is the offending type + the
// §3.6/§9.2/§35.7 reason — the analog of sendableFailureComponentPath, at the
// detail level §3.6 supports for this property. Returns empty (the caller keeps
// the flat message) when the type is not the record/choice failure shape.
std::string hashableFailureComponentPath(const TypeRef* ty,
                                         const SemaContext& ctx) {
    if (!ty || ty->kind != TypeRef::Kind::Named || !ty->declaration) return {};
    const TypeSymbol* ts = ty->declaration;
    // Fragment prefix — reads naturally after the parent §14.2 citation:
    // "...does not satisfy the constraint `where T: hashable` (§14.2): ...".
    const std::string prefix = ": ";
    if (ts->kind == ast::TypeDeclKind::Choice) {
        auto verdict = common::classifyChoiceType(ty, ctx);
        if (!verdict) return {};  // unclassifiable (imported) — flat reason
        if (*verdict == common::ChoiceClassification::ValueLike) {
            // Value-like yet still failed isEqualityBearing → the RECURSIVE
            // §35.7 rule rejected a payload (v0.210/#2083 merged rule; the
            // rework's finding 3): name the offending case/payload component
            // so the diagnostic carries the §3.6 rule-4 component path.
            if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl)) {
                for (const auto& cc : td->choiceCases) {
                    for (const auto& pf : cc.payload) {
                        if (pf.modifier != "owned" && pf.type &&
                            isAdmissibleComponentType(
                                ctx.resolvedType(*pf.type), ctx, 0))
                            continue;
                        return prefix + "choice `" + std::string(ts->name) +
                               "` is value-like but case `" +
                               std::string(cc.name) + "` payload `" +
                               std::string(pf.name) +
                               "` is not an admissible equality component "
                               "(§35.7/§9.2)";
                    }
                }
            }
            return {};  // no offender found — flat reason (defensive)
        }
        return prefix + "choice `" + std::string(ts->name) + "` is " +
               (*verdict == common::ChoiceClassification::Owning
                    ? "owning"
                    : "borrow-bearing") +
               " (§35.7), not value-like auto-equality-bearing";
    }
    if (ts->kind == ast::TypeDeclKind::Record) {
        return prefix + "record `" + std::string(ts->name) +
               "` is neither auto-equality-bearing (§9.2) nor declares custom "
               "equality (`operator =`/`<>` with `Equals(T)`/`GetHashCode`, "
               "§25.2)";
    }
    return {};  // class/interface/enum/primitive are all equality-bearing —
                // not reached under the precondition.
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
