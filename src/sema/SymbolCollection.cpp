#include "sema/SymbolCollection.hpp"

#include "ast/Node.hpp"
#include "common/BuiltinRegistry.hpp"  // isBuiltinExceptionName — §31.4 (#2950)
#include "common/OwnershipFacts.hpp"  // classifyChoiceType — §35.7 hashable choice edge (#2083/#2087)
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

#include <unordered_set>  // inheritsBuiltinInterface's cycle guard (#3139)
#include <vector>

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
// at some use sites and not others (the #1081 wrong-value bug). The lowercase
// keywords cannot reach a type-decl name (the lexer tokenizes them as keywords,
// not identifiers).
//
// fix #5187: `Object` is correctly ABSENT from this list, but it is not an
// unreserved identifier either. §3.4's reserved-name rule rests on the
// keyword/PascalCase dual form, and `Object` has no lowercase keyword alias
// (spec §3.4: "there is no `object` spelling"), so this predicate is the wrong
// home for it. Its reservation is a separate, narrower rule — §8.1 (v0.311,
// unchanged at v0.313): "User code cannot redeclare or replace `Object`. The
// ban is on a user module-level type declaration — a class, record, enum,
// interface, choice or type alias — named `Object` with no type parameters,
// in any user module and at any visibility." §8.1 permits every other
// declaration of the name (a generic `Object<T>` is a different name, §2.2
// arity keying; a nested type or type parameter shadows the root under §2.7).
// That check is emitted at the two sites that see the whole declaration shape:
// collectTypeDecl (guarded on
// `enclosingTypePath.empty() && genericParams.empty()`) and collectTopDecl's
// TypeAlias branch. A method-local `Object` is §21.10's error, a separate rule.
//
// fix #3833 (ruled #3812, v0.293): `Single` is NOT in this list. §3.4's table
// has no `Single` row and its reserved-name list does not name it; the
// resolver's undocumented `Single` -> float arm (that reservation rode on) is
// deleted in TypeResolution.cpp, so `Single` is an ordinary, unreserved
// identifier again — a user type may now be declared `Single`.
bool isReservedPrimitiveAliasName(std::string_view n) {
    return n == "Boolean" || n == "Byte" || n == "SByte" || n == "Int16" ||
           n == "UInt16"  || n == "Int32" || n == "UInt32" || n == "Int64" ||
           n == "UInt64"  || n == "Float" || n == "Double" ||
           n == "Decimal" || n == "Char"  || n == "String";
}

// Forward declarations for the recursive descent.
// Note: collectTypeDecl is defined OUTSIDE the anonymous namespace (see below)
// so it does not need a forward declaration here.
Symbol* collectTopDecl(const ast::TopDecl& decl, SemaContext& ctx,
                       ScopeId parentScope);
Symbol* collectMember(const ast::Member& m, TypeSymbol& parent,
                      SemaContext& ctx);
Symbol* collectMemberImpl(const ast::Member& m, TypeSymbol& parent,
                          SemaContext& ctx);

// A.3 GenericParam: register one symbol per declared parameter in the
// given scope.
//
// fix #3146 (§14.2, v0.252 — family F13 of umbrella #3152): `constraints` is
// the declaring node's `where` clauses, which are keyed by parameter NAME
// (`ast::GenericConstraint::typeParam`). Re-keying them onto the parameter
// SYMBOL here is the load-bearing half of the fix: it is what lets the three
// §14.2 readers ask the parameter rather than the declaring `ast::TypeDecl`,
// which is null for every imported type. Null for a declaration with no
// `where` clause at all.
std::vector<Symbol*> collectGenericParams(
    const std::vector<ast::GenericParam>& params,
    SemaContext& ctx, ScopeId scope,
    const std::vector<ast::GenericConstraint>* constraints = nullptr) {
    std::vector<Symbol*> out;
    out.reserve(params.size());
    for (const auto& p : params) {
        auto* sym = ctx.makeSymbol<GenericParamSymbol>();
        sym->name = p.name;
        sym->variance = p.variance;
        sym->scope = scope;
        if (constraints) {
            for (const ast::GenericConstraint& gc : *constraints) {
                if (gc.typeParam != p.name) continue;
                // §14.4: the historical `where T is owned` / `is not owned`
                // form carries no instantiation-time obligation; every reader
                // skipped such a clause, and its `items` are empty anyway.
                if (!gc.modifierConstraint.empty()) continue;
                for (const ast::ConstraintItem& ci : gc.items) {
                    if (ci.kind.empty()) continue;
                    GenericParamConstraint c;
                    c.kind = ci.kind;
                    c.astBound = ci.typeArg.get();  // null for a scalar kind
                    sym->constraints.push_back(c);
                }
            }
        }
        out.push_back(sym);
    }
    return out;
}

// A.4 Param list: register one symbol per declared parameter.
std::vector<Symbol*> collectParams(const std::vector<ast::Param>& params,
                                   SemaContext& ctx, ScopeId scope) {
    std::vector<Symbol*> out;
    out.reserve(params.size());
    // fix #5367 (§22.1, v0.314): the names in one parameter list are distinct.
    // Track seen names and report an error at the second occurrence.  The symbol
    // is still registered so later passes have a well-formed type to resolve
    // against; the diagnostic makes the program a compile error.
    std::unordered_set<std::string_view> seenNames;
    for (const auto& p : params) {
        if (!p.name.empty() && p.name != "_") {
            if (seenNames.count(p.name) > 0) {
                ctx.diagnostics().report(
                    Severity::Error, p.loc,
                    "duplicate parameter name `" + std::string(p.name) +
                        "` in this parameter list (§22.1, v0.314)");
            } else {
                seenNames.insert(p.name);
            }
        }
        auto* sym = ctx.makeSymbol<ParameterSymbol>();
        sym->name = p.name;
        sym->modifier = p.modifier;
        sym->scope = scope;
        sym->declType = p.type.get();
        // §22.4 (v0.183, #1585): carry the out-nullability postcondition onto
        // the symbol so narrowing and override-match can consult it.
        sym->outNullPostcond = p.outNullPostcond;
        // fix #3178 / §22.5 (v0.254): carry "declares a default" onto the symbol
        // for a LOCAL formal too, so `formalParamHasDefault` gives the same
        // answer whichever source it reaches for. The accessor still prefers the
        // AST (which carries the default EXPRESSION, not just its presence), so
        // this is redundancy that makes the two sources agree by construction
        // rather than a second answer that can drift.
        sym->hasDefault = (p.defaultValue != nullptr);
        // fix #3037 / §28.3 (v0.240): carry the parameter's own source
        // location onto the symbol. The parser stamps `Param::loc` at the
        // parameter's first token (modifier or name) — `Param` is not an
        // `ast::Node` so it has no `span`, only this single SourceLocation.
        // Without it, every §28.3 owned-parameter diagnostic that doesn't have
        // a recorded `obligationOrigin` (the parameter arm — the obligation
        // is established at the parameter declaration itself, not at an
        // expression) would have fallen back to `SourceLocation{}` and
        // printed `0:0`, pointing at nothing (the parameter is the only
        // owned-binding kind whose diagnostic site is its declaration, not
        // its first use site; the param arm has no "first use"). Mirrors
        // fix #2418 (FieldSymbol/MethodSymbol/etc.) and fix #2482
        // (PositionalField via FieldSymbol::declLoc).
        sym->declLoc = (p.loc.line != 0) ? p.loc : sym->declLoc;
        out.push_back(sym);
    }
    return out;
}

Symbol* collectMemberImpl(const ast::Member& m, TypeSymbol& parent,
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
        // §9.2 "The `equality skip` clause" (v0.307): an explicit field's
        // clause, onto the SYMBOL. A.4 places it BEFORE the identifier on a
        // FieldDecl; this is the third of the three storage-bearing AST hosts
        // (the other two are the positional field in collectTypeDecl and the
        // auto-property below). Copied unconditionally — including onto a
        // `static` field, which §9.2's inert-annotation rule then REJECTS in
        // `Resolver::checkTypeEqualitySkipInert`; that check reads the symbol
        // bit, so dropping it here for statics would silently disarm the rule.
        if (fd->equality) sym->equalitySkip = fd->equality->isSkip;
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
            collectGenericParams(md->genericParams, ctx, scope,
                                 &md->genericConstraints);
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
            // fix #5868 (§23.4 / §1.6, v0.334): carry `blockEndLabel` onto
            // the symbol so `checkIndexerEndLabel` can consult it without
            // walking the AST decl — the AST walk is null for an imported
            // indexer and the check would skip silently.
            sym->blockEndLabel = pd->blockEndLabel;
        }
        // fix #3618 (§22.15 "Purity attaches to the getter"): the
        // accessor-level `pure get` marker. Mirrors
        // `propertyGetterIsPure`'s own accessor arm — any accessor with
        // `isPure && !isSet`. Storing it on the symbol means
        // `propertyGetterIsPure` has a same-shape fallback for the LOCAL
        // side (the AST walk would agree), and — critically — gives the
        // IMPORTED side a place to receive the same fact from the wire
        // (ModuleLoader Property case sets `sym->accessorIsPure =
        // mm.accessorIsPure`). Indexer's PropertyAccessor list is the
        // parallel shape — `get`/`set` are members of the same vector —
        // so this loop covers indexers identically.
        for (const auto& a : pd->accessors)
            if (a.isPure && !a.isSet) {
                sym->accessorIsPure = true;
                break;
            }
        // fix #5209 (§23.3, v0.313): `set(owned)` — see PropertySymbol::
        // setterTakesOwned.
        for (const auto& a : pd->accessors)
            if (a.isSet && a.setterParamMod == "owned") {
                sym->setterTakesOwned = true;
                break;
            }
        // fix #3630 narrow (§22.15 v0.286, over-reject follow-up): LOCAL
        // twin of the ModuleLoader Property case's
        // `p->isAutoProperty = mm.isAutoProperty`. The exporter writes
        // `out.isAutoProperty` for a non-static, non-abstract auto-property
        // (bodyKind == Auto, no static/abstract modifier — see
        // ModuleMetadataBuilder.cpp's PropertySymbol arm); the LOCAL sema
        // arm mirrors it via `hasAstAutoPropertyDecl`, the same predicate
        // the storage-classification code paths use. Without this,
        // `propertyGetterIsPure` answers "false" for a LOCAL auto-property
        // and #3630's MemberAccessExpr arm over-rejects the read in a pure
        // body — the regression measured on xmod_pure_ctor_chain_3174.t5b
        // and xmod_record_sibling_ctor_3219.t1b.
        sym->isAutoProperty = hasAstAutoPropertyDecl(sym);
        // fix #3140 (§9.8, family F2 of umbrella #3152): an auto-property's
        // `serialized` clause — the second of the two storage-bearing sites the
        // A.4 grammar admits it on — onto the SYMBOL, for the same reason the
        // positional-field copy in collectTypeDecl gives.
        if (pd->serialized) {
            sym->serializedSkip     = pd->serialized->isSkip;
            sym->serializedOptional = pd->serialized->isOptional;
            sym->serializedWireName = pd->serialized->name;
        }
        // §9.2 "The `equality skip` clause" (v0.307): an auto-property's
        // clause, onto the SYMBOL, for the same reason the `serialized` copy
        // directly above gives — §9.2 states the artifact interface carries
        // it "exactly as it carries `serialized` clauses".
        if (pd->equality) sym->equalitySkip = pd->equality->isSkip;
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
            // fix #3529 (§11.6 / §2.8.3 rule 3, v0.283): thread the
            // ENCLOSING-TYPE PATH down so the inner TypeSymbol carries the
            // other half of rule 3's identity ("its qualified type name
            // (including any enclosing-type path, §11.6)"). `parent`'s own
            // path is already populated — collectTypeDecl assigns it before
            // it walks the member list — so a three-deep `A.B.C` composes
            // correctly rather than each level seeing only its immediate
            // parent. See TypeSymbol::enclosingTypePath for what the missing
            // half cost the emitted symbol namespace.
            wrapper->type =
                collectTypeDecl(*nt->decl, ctx, parent.memberScope,
                                parent.arityEnclosingPath());
        }
        return wrapper;
    }
    // Unknown Member kind — return null so the caller can skip without
    // diagnosing (parser would already have errored if it built one).
    return nullptr;
}

// fix #3609 (§22.15 / §2.8.2, v0.286): fill the SYMBOL-side purity channel for
// a LOCAL declaration, so `MemberSymbol::isPure` means the same thing on both
// sides of the artifact boundary instead of meaning "imported AND pure".
//
// THE RULE IS THE EXPORTER'S, VERBATIM. `ModuleMetadataBuilder::convertMember`
// writes the wire as `out.isPure = hasModifier(m.modifiers, "pure")`, then its
// operator arm overrides with `od->isPure` and its constructor arm with
// `cd->isPure` — the two forms whose `pure` the parser consumes into a
// dedicated AST bool instead of leaving in the modifier list
// (Parser_Decls.cpp, parseOperatorDeclBody / the ctor arm). Mirroring it
// exactly is what makes the channel a channel: a member exported by this
// compiler and re-imported reads back the value it had here. (The exporter is
// deliberately left reading the declaration rather than this field: it is
// SERIALISING a declaration, not answering §22.15, and it must keep working for
// the member symbols that are not built by `collectMember` — see the note on
// the wrapper below. That also keeps the emitted artifact bytes provably
// unchanged by this fix.)
//
// It admits nothing new on its own. Every reader of the field ORs it with the
// modifier scan it duplicates (`memberIsPure` below, and the two PureChecker
// sites that predate it), so a local member that now answers `true` through
// `isPure` already answered `true` through `modifiers` or through its decl.
static bool declaredPure(const MemberSymbol& m) {
    // An operator's and a constructor's `pure` exist ONLY on the AST node:
    // neither `ast::OperatorDecl` nor `ast::ConstructorDecl` keeps the modifier
    // list the parser scanned for it (Parser_Decls.cpp), so the modifier arm
    // below cannot answer for either form. This function runs on a LOCAL
    // declaration only — `ModuleLoader::applyBehavior` fills the same field
    // from the wire for an import — so a null cast here is the expected shape,
    // not a silent import miss.
    // SEAM-ACCESSOR: the operator arm of the local member-purity channel.
    if (const auto* od = dynamic_cast<const ast::OperatorDecl*>(m.decl))
        return od->isPure;
    // SEAM-ACCESSOR: the constructor arm of the local member-purity channel.
    if (const auto* cd = dynamic_cast<const ast::ConstructorDecl*>(m.decl))
        return cd->isPure;
    for (std::string_view mod : m.modifiers)
        if (mod == "pure") return true;
    return false;
}

Symbol* collectMember(const ast::Member& m, TypeSymbol& parent,
                      SemaContext& ctx) {
    Symbol* sym = collectMemberImpl(m, parent, ctx);
    // One place, every member kind — the arms of `collectMemberImpl` each
    // `return` early, so filling the field per-arm is how the next kind gets
    // forgotten. §22.15 rejects `pure` on a field, event, destructor, const and
    // enum constant, so for those kinds this is `false` in every well-formed
    // program; it is still assigned uniformly, because the wire assigns it
    // uniformly and a channel that agrees only on the reachable kinds is the
    // shape this fix exists to remove.
    //
    // ⚠ NOT every `MemberSymbol` is built here, and `memberIsPure` keeps its
    // modifier and decl arms precisely so this field is never the ONLY source.
    // The builtin-interface shims (SemaContext.hpp) and a record's synthesized
    // positional fields (collectTypeDecl below) construct symbols directly and
    // leave it `false`; none of them can carry `pure` — they have no modifier
    // list to put it in and no `ast::OperatorDecl`/`ast::ConstructorDecl` — so
    // the predicate still answers correctly for them.
    if (auto* mem = dynamic_cast<MemberSymbol*>(sym))
        mem->isPure = declaredPure(*mem);
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
        // fix #5187 (spec §8.1, v0.311, unchanged at v0.313): a top-level
        // type alias named `Object` is the same reserved-name violation as
        // a top-level class/record/enum/interface/choice named `Object`.
        // §8.1 is the authority and names the alias form itself in its own
        // terms: "User code cannot redeclare or replace `Object`. The ban is
        // on a user module-level type declaration — a class, record, enum,
        // interface, choice or type alias — named `Object` with no type
        // parameters, in any user module and at any visibility." The alias
        // is a top-level declaration by §27.8 ("Type aliases may be declared
        // at module level. A module-level alias is always **module-internal**"),
        // so it lands inside §8.1's ban with no separate rule required.
        // The symbol is still registered so later passes have a well-formed
        // type to resolve against; the diagnostic makes the program a
        // compile error. Generic type-parameter aliases (e.g.
        // `type Object<T> := List<T>;`) are outside the ban per §8.1's own
        // carve-out ("a generic `Object<T>` is a different name", §2.2), the
        // same rule that exempts `public class Object<T>`.
        if (alias->name == "Object" && alias->genericParams.empty()) {
            ctx.diagnostics().report(
                Severity::Error, alias->span.begin,
                std::string("`") + std::string(alias->name) +
                "` is reserved for the root type and may not be redeclared "
                "as a user module-level type alias (§8.1) — choose a "
                "different name");
        }
        sym->decl = alias;
        sym->declLoc = alias->span.begin;  // fix #2418: TypeAlias : TopDecl : Node
        sym->scope = parentScope;
        const ScopeId aliasScope = ctx.newScope();
        sym->genericParams =
            collectGenericParams(alias->genericParams, ctx, aliasScope,
                                 &alias->genericConstraints);
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
        // fix #2335 (§2.8.3 rule 3 / §26, v0.315): record the declaring module
        // so `Emitter::functionName` can qualify primitive-target extension
        // method mangling with the extension's own module, not just the target's.
        sym->moduleName = moduleDottedName(ctx.module());
        ctx.registerExtensionForDecl(ext, sym);
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
            // §32.11: carry the alias and variadic clause from the AST.
            dsym->symbolAlias = d.symbolAlias;
            dsym->variadicN = d.variadicN;
            // §32.11 No silent promotions: variadic-position parameters must
            // already have a promoted type. The non-promoted set: float, bool,
            // byte, sbyte, short, ushort, string, char, decimal, and non-scalar
            // records. Diagnostics cite the parameter and the promoted type.
            if (d.variadicN != UINT32_MAX) {
                // §32.11: "`N` ≤ the declared arity." A `variadic(N)` naming
                // more fixed parameters than the declaration has is not a
                // spelling of anything; it used to fall out of the loop below
                // vacuously and reach codegen as "every parameter is
                // variadic", i.e. a silent acceptance of a value the source
                // never spelled.
                if (d.variadicN > d.params.size()) {
                    ctx.diagnostics().report(
                        Severity::Error, d.loc,
                        "variadic(" + std::to_string(d.variadicN) +
                            ") names more fixed parameters than the "
                            "declaration has (" +
                            std::to_string(d.params.size()) +
                            "); §32.11 requires N ≤ the declared arity");
                }
                for (std::size_t i = d.variadicN; i < d.params.size(); ++i) {
                    const auto& p = d.params[i];
                    if (!p.type) continue;
                    const auto* ty = p.type.get();
                    bool isPromoted = false;
                    std::string typeName;
                    std::string promotedReplacement;
                    if (ty->kind == ast::TypeKind::Named && ty->name &&
                        ty->name->parts.size() == 1) {
                        typeName = std::string(ty->name->parts[0]);
                        // promoted scalar types
                        if (typeName == "int" || typeName == "uint" ||
                            typeName == "long" || typeName == "ulong" ||
                            typeName == "double") {
                            isPromoted = true;
                        }
                        // promoted pointer types
                        else if (typeName == "IntPtr" || typeName == "UIntPtr" ||
                                 typeName == "CString" || typeName == "WString") {
                            isPromoted = true;
                        }
                        // Ptr<T> is promoted (single generic arg)
                        else if (typeName == "Ptr" && !ty->genericArgs.empty()) {
                            isPromoted = true;
                        }
                        // non-promoted: map to the promoted replacement
                        else if (typeName == "float") {
                            promotedReplacement = "double";
                        } else if (typeName == "bool" || typeName == "byte" ||
                                   typeName == "ushort") {
                            promotedReplacement = "int or uint";
                        } else if (typeName == "sbyte" || typeName == "short") {
                            promotedReplacement = "int or long";
                        } else if (typeName == "RawBytes") {
                            // §32.11: RawBytes occupies two C slots (ptr, len) and
                            // does not fit the one-slot-per-parameter promotion
                            // accounting — must be passed with an explicit length.
                            promotedReplacement = "Ptr<byte> or IntPtr plus an explicit length";
                        } else if (typeName == "string" || typeName == "char" ||
                                   typeName == "decimal") {
                            // §32.11: not in the promoted type list
                            promotedReplacement = "pointer type (Ptr<byte>, IntPtr, CString, etc.)";
                        }
                    }
                    // Function types are also promoted
                    else if (ty->kind == ast::TypeKind::Function) {
                        isPromoted = true;
                    }
                    if (!isPromoted && !promotedReplacement.empty()) {
                        ctx.diagnostics().report(
                            Severity::Error, p.loc,
                            "parameter `" + std::string(p.name) +
                                "` has type `" + typeName +
                                "` in a variadic position — this type is not "
                                "silently promoted in C variadic calls; " +
                                typeName + " promotes to " +
                                promotedReplacement +
                                " (§32.11 No silent promotions; declare the "
                                "parameter as " + promotedReplacement + ")");
                    }
                }
            }
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

// fix #2198 (§21.10, v0.310): moved outside the anonymous namespace so it can
// be called from other translation units (TypeResolution_Names.cpp). The
// anonymous-namespace helpers it calls (collectMember, collectGenericParams,
// moduleDottedName) are still in scope and accessible from here.
// Added `inline` so that calls from within the anonymous namespace (which has
// its own forward declaration) resolve to THIS definition rather than creating
// an ambiguity with the anonymous-namespace declaration.
inline TypeSymbol* collectTypeDecl(const ast::TypeDecl& td, SemaContext& ctx,
                            ScopeId parentScope,
                            std::string_view enclosingTypePath) {
    auto* sym = ctx.makeSymbol<TypeSymbol>();
    sym->name = td.name;
    sym->enclosingTypePath = std::string(enclosingTypePath);
    if (isReservedPrimitiveAliasName(td.name)) {
        ctx.diagnostics().report(
            Severity::Error, td.span.begin,
            std::string("`") + std::string(td.name) +
            "` is a reserved primitive-alias type name (§3.4) and may not be "
            "declared as a user type — it names a built-in primitive value type "
            "through its lowercase keyword alias; choose a different name");
    }
    if (td.name == "Object" && enclosingTypePath.empty() &&
        td.genericParams.empty()) {
        ctx.diagnostics().report(
            Severity::Error, td.span.begin,
            std::string("`") + std::string(td.name) +
            "` is reserved for the root type and may not be redeclared "
            "as a user module-level type (§8.1) — choose a different name");
    }
    sym->kind = td.kind;
    sym->visibility = td.visibility;
    sym->modifiers = td.modifiers;
    sym->decl = &td;
    sym->moduleName = moduleDottedName(ctx.module());
    sym->declLoc = td.span.begin;
    sym->scope = parentScope;
    sym->memberScope = ctx.newScope();
    sym->genericParams =
        collectGenericParams(td.genericParams, ctx, sym->memberScope,
                             &td.genericConstraints);
    for (const auto& m : td.members) {
        if (!m) continue;
        if (auto* member = collectMember(*m, *sym, ctx)) {
            sym->members.push_back(member);
        }
    }
    sym->hasPositionalFields = td.hasPositionalFields;
    if (td.hasPositionalFields) {
        for (const auto& pf : td.positionalFields) {
            auto* fs = ctx.makeSymbol<FieldSymbol>();
            fs->name = pf.name;
            fs->visibility = ast::Visibility::Public;
            fs->parent = sym;
            fs->decl = &td;
            fs->declLoc = (pf.loc.line != 0) ? pf.loc : sym->declLoc;
            fs->posParam = &pf;
            fs->scope = sym->memberScope;
            if (pf.serialized) {
                fs->serializedSkip     = pf.serialized->isSkip;
                fs->serializedOptional = pf.serialized->isOptional;
                fs->serializedWireName = pf.serialized->name;
            }
            if (pf.equality) fs->equalitySkip = pf.equality->isSkip;
            sym->members.push_back(fs);
        }
    }
    for (const auto& ec : td.enumConstants) {
        auto* esym = ctx.makeSymbol<EnumConstantSymbol>();
        esym->name = ec.name;
        esym->parent = sym;
        esym->declLoc = (ec.loc.line != 0) ? ec.loc : sym->declLoc;
        esym->scope = sym->memberScope;
        if (ec.serialized && !ec.serialized->name.empty())
            esym->serializedName = ec.serialized->name;
        sym->members.push_back(esym);
    }
    for (const auto& cc : td.choiceCases) {
        auto* csym = ctx.makeSymbol<ChoiceCaseSymbol>();
        csym->name = cc.name;
        csym->parent = sym;
        csym->caseDecl = &cc;
        csym->declLoc = (cc.loc.line != 0) ? cc.loc : sym->declLoc;
        csym->scope = sym->memberScope;
        sym->members.push_back(csym);
    }
    return sym;
}

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

// fix #4924 (§22.3 / §22.4 / §22.6, v0.307): see Symbol.hpp.
bool callSiteRefOutMatches(std::string_view argModifier,
                           std::string_view paramModifier) {
    const bool argRef = argModifier == "ref";
    const bool argOut = argModifier == "out" || argModifier == "out var" ||
                        argModifier == "out using var" ||
                        argModifier == "out owned var";  // per §22.4
    if (paramModifier == "ref") return argRef;
    if (paramModifier == "out" || paramModifier == "out owned") return argOut;
    return !argRef && !argOut;
}

std::size_t formalParamCount(const ConstructorSymbol& cs) {
    if (auto* d = dynamic_cast<const ast::ConstructorDecl*>(cs.decl))
        return d->params.size();
    return cs.params.size();
}

// narrow #4791 (§12.8 / §22.5 / §22.10, v0.307): see Symbol.hpp. Each
// conjunct below mirrors one gate of the #3793 self-chain check in
// `Resolver::checkTypeConstructorChains`, in the same order, plus the one
// new conjunct at the end.
const ast::CallExpr* deferredNamedThisChain(const TypeSymbol& t,
                                            const ConstructorSymbol& cs,
                                            const ast::ConstructorDecl& cd) {
    // #3793 applies only to a type WITHOUT a primary constructor — a
    // §22.10 class primary or, fix #4999 (§9.1 / §9.3, v0.307), a record's
    // §9.1 auto-generated primary; the same predicate as
    // `Resolver::checkTypeConstructorChains`'s `typeHasPrimaryCtor`.
    if (t.hasPositionalFields && (t.kind == ast::TypeDeclKind::Class ||
                                  t.kind == ast::TypeDeclKind::Record))
        return nullptr;
    if (cs.isStatic || cd.body.empty() || !cd.body[0]) return nullptr;
    // A misplaced or duplicated chain call is already a §12.8 error.
    int chainCount = 0;
    for (std::size_t i = 0; i < cd.body.size(); ++i) {
        bool isThisI = false;
        if (!cd.body[i] || !ast::chainCallOfStmt(*cd.body[i], isThisI))
            continue;
        if (i != 0) return nullptr;
        ++chainCount;
    }
    if (chainCount > 1) return nullptr;
    bool isThis = false;
    const ast::CallExpr* chain = ast::chainCallOfStmt(*cd.body[0], isThis);
    if (!chain || !isThis) return nullptr;
    const std::size_t n = chain->args.size();
    if (n != cs.params.size()) return nullptr;
    for (Symbol* m : t.members) {
        auto* other = dynamic_cast<ConstructorSymbol*>(m);
        if (!other || other->isStatic || other == &cs) continue;
        if (other->params.size() == n) return nullptr;
    }
    // The new conjunct: a named argument naming no parameter of `cs`.
    const std::size_t csArity = formalParamCount(cs);
    for (const auto& a : chain->args) {
        if (a.name.empty()) continue;
        bool namesCsParam = false;
        for (std::size_t j = 0; j < csArity; ++j)
            if (formalParamName(cs, j) == a.name) {
                namesCsParam = true;
                break;
            }
        if (!namesCsParam) return chain;
    }
    return nullptr;
}

// fix #3176 (§22.3 / §2.5, v0.254 — family M1 of umbrella #3152): the METHOD
// twin of the constructor accessors above. See Symbol.hpp for the contract and
// for the measured heap-use-after-free that motivates it.
namespace {
// The AST formal list of a locally declared method. `MethodSymbol` also backs
// `operator` overloads, whose decl is an `ast::OperatorDecl` (not derived from
// `ast::MethodDecl`) — both carry the same `std::vector<ast::Param> params`, so
// both are the local source.
const std::vector<ast::Param>* methodDeclParams(const MethodSymbol& ms) {
    // SEAM-ACCESSOR: the local source of the formalParam* accessors; the AST arm
    // is the point of it.
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms.decl))
        return &md->params;
    // SEAM-ACCESSOR: the operator half of the same question.
    if (auto* od = dynamic_cast<const ast::OperatorDecl*>(ms.decl))
        return &od->params;
    return nullptr;
}

const ast::Param* methodDeclParam(const MethodSymbol& ms, std::size_t pos) {
    const std::vector<ast::Param>* ps = methodDeclParams(ms);
    if (!ps || pos >= ps->size()) return nullptr;
    return &(*ps)[pos];
}

const ParameterSymbol* methodSymParam(const MethodSymbol& ms, std::size_t pos) {
    if (pos >= ms.params.size()) return nullptr;
    return dynamic_cast<const ParameterSymbol*>(ms.params[pos]);
}
} // namespace

std::string_view formalParamModifier(const MethodSymbol& ms, std::size_t pos) {
    if (const ast::Param* p = methodDeclParam(ms, pos)) return p->modifier;
    if (const ParameterSymbol* p = methodSymParam(ms, pos)) return p->modifier;
    return {};
}

std::string_view formalParamName(const MethodSymbol& ms, std::size_t pos) {
    if (const ast::Param* p = methodDeclParam(ms, pos)) return p->name;
    if (const ParameterSymbol* p = methodSymParam(ms, pos)) return p->name;
    return {};
}

std::size_t formalParamCount(const MethodSymbol& ms) {
    if (const std::vector<ast::Param>* ps = methodDeclParams(ms))
        return ps->size();
    return ms.params.size();
}

const ast::Type* formalParamDeclType(const MethodSymbol& ms, std::size_t pos) {
    if (const ast::Param* p = methodDeclParam(ms, pos)) return p->type.get();
    if (const ParameterSymbol* p = methodSymParam(ms, pos)) return p->declType;
    return nullptr;
}

const TypeRef* formalParamResolvedType(const MethodSymbol& ms,
                                       std::size_t pos) {
    if (const ParameterSymbol* p = methodSymParam(ms, pos))
        return p->resolvedDeclType;
    return nullptr;
}

// fix #3189 (§28.1 / §28.3 / §2.5, v0.254 — family M2 of umbrella #3152): THE
// both-sources reader of a method's `owned` return marker. See Symbol.hpp for
// the contract and for the four measured cross-module defects it closes.
bool methodReturnsOwned(const MethodSymbol& ms, const SemaContext& ctx) {
    // AST first (it carries the syntax), symbol/side-table second — the same
    // order as the formalParam* accessors above, so a member described by both
    // sources can never get two answers.
    // SEAM-ACCESSOR: this IS the both-sources accessor for "does it return
    // owned?" — the AST arm is required, not an offender. Exempted per-site
    // rather than by whitelisting this file, because only some of
    // SymbolCollection.cpp's member reads are accessor arms.
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms.decl))
        return md->returnOwned;
    // SEAM-ACCESSOR: the operator half of the same question.
    if (auto* od = dynamic_cast<const ast::OperatorDecl*>(ms.decl))
        return od->returnOwned;
    return ctx.importedReturnOwned(&ms);
}

// fix #4988 (§22 / §27 / §28.3, v0.307 — sister accessor to
// `methodReturnsOwned` for umbrella #3152): the both-sources reader of a
// method's DECLARED RETURN TYPE. Mirrors `methodReturnsOwned`'s contract:
// AST (MethodDecl/OperatorDecl) first, then the loader's `importedReturnType`
// side-table. The five hand-written copies this accessor retires (see
// Symbol.hpp for the enumeration) each spelled the same dual-sourcing pattern
// in their own way — the consolidation drains the class-3 SEAM-DEBT entries
// at every site that was carrying one for #4988.
//
// AST-first order matches `methodReturnsOwned` above and `formalParam*` above
// that: for an imported GENERIC type the loader sets BOTH `decl` (body
// re-parse) AND the `importedReturnType` wire entry, and the AST arm wins
// (i.e. this accessor changes nothing for that case). For an imported
// non-generic class member the AST arm is null and the loader-recorded entry
// is the single source of truth.
TypeRef* methodReturnResolvedType(const MethodSymbol& ms,
                                  const SemaContext& ctx) {
    // SEAM-ACCESSOR: AST arm for a regular method declaration. The
    // `returnType` AST node carries the syntactic shape; sema's
    // `resolveTypes` pass populates `ctx.resolvedTypes_` for it (so the
    // lookup below returns null only for an unresolved body — a caller
    // that consumes the accessor against an unresolved AST would see null,
    // matching the hand-written copy's behaviour at the same stage).
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms.decl))
        return md->returnType ? ctx.resolvedType(*md->returnType) : nullptr;
    // AST arm for an operator overload — `OperatorDecl` is a sibling of
    // `MethodDecl`, not a subclass, so the audit's `methodReturnsOwned`
    // accessor above already had to consider it; this sibling mirrors the
    // same split. Before this accessor existed the shared-event-handler
    // site (#4901) silently rejected an OperatorDecl here via its
    // `else if (ms->decl) return false;` defensive guard — the
    // consolidation treats operators the same as methods, which is the
    // substantive claim the seam-audit intended.
    // SEAM-ACCESSOR: the operator half of the same both-sources question.
    if (auto* od = dynamic_cast<const ast::OperatorDecl*>(ms.decl))
        return od->returnType ? ctx.resolvedType(*od->returnType) : nullptr;
    // SEAM-ACCESSOR: the imported half. ModuleLoader sets `decl` only where
    // it re-parsed exported body text for a GENERIC type; for any
    // non-generic imported class member the AST arm is null and the
    // loader-recorded wire return on `ctx.importedReturnType` is the single
    // source of truth. A side-table miss returns null, which the existing
    // hand-written copies already treated as "no constraint" / default-to-
    // unit — the accessor preserves that interpretation.
    return ctx.importedReturnType(&ms);
}

// fix #5042 (§24 / stdlib §6.5, v0.313; #4968 seam debt, umbrella #3152): the
// both-sources readers of an `event` member's handler slot. Local events answer
// off the `ast::EventDecl`; an imported event has no decl (ModuleLoader never
// reconstructs one) and answers off the loader's wire side tables
// (`importedMemberType` = payload, `importedEventIsAsync` = the `async` bit
// carried by MetaMember::isEventAsync, #2421).
bool eventHandlerIsAsync(const EventSymbol& es, const SemaContext& ctx) {
    // SEAM-ACCESSOR: the local half; `async` lives on the EventDecl only.
    if (auto* ed = dynamic_cast<const ast::EventDecl*>(es.decl))
        return ed->isAsync;
    // SEAM-ACCESSOR: the imported half, the wire-carried async bit.
    return ctx.importedEventIsAsync(&es);
}

bool eventPayloadResolvedType(const EventSymbol& es, const SemaContext& ctx,
                              TypeRef*& payload) {
    payload = nullptr;
    // SEAM-ACCESSOR: the local half; the `: T` annotation on the EventDecl.
    if (auto* ed = dynamic_cast<const ast::EventDecl*>(es.decl)) {
        if (!ed->type) return true;  // no payload: the handler is `() -> ()`
        payload = ctx.resolvedType(*ed->type);
        return payload != nullptr;   // unresolved: reported elsewhere
    }
    if (es.decl != nullptr) return false;
    // The imported half: a Unit wire type is the exporter's encoding of an
    // omitted `: T` (#3211).
    TypeRef* rt = ctx.importedMemberType(&es);
    if (!rt) return false;
    if (rt->kind != TypeRef::Kind::Unit) payload = rt;
    return true;
}

const ast::Type* formalParamDeclType(const ConstructorSymbol& cs,
                                     std::size_t pos) {
    if (const ast::Param* p = ctorDeclParam(cs, pos)) return p->type.get();
    if (const ParameterSymbol* p = ctorSymParam(cs, pos)) return p->declType;
    return nullptr;
}

const TypeRef* formalParamResolvedType(const ConstructorSymbol& cs,
                                       std::size_t pos) {
    if (const ParameterSymbol* p = ctorSymParam(cs, pos))
        return p->resolvedDeclType;
    return nullptr;
}

// fix #3178 (§22.5 "Optional parameters" / §2.5, v0.254 — family M5 of umbrella
// #3152). See Symbol.hpp for the contract. Same AST-first order as every other
// formal accessor; `collectParams` now sets `ParameterSymbol::hasDefault` for a
// local formal too, so the two sources agree by construction and the order is
// documentation rather than a tie-break.
bool formalParamHasDefault(const MethodSymbol& ms, std::size_t pos) {
    if (const ast::Param* p = methodDeclParam(ms, pos))
        return p->defaultValue != nullptr;
    if (const ParameterSymbol* p = methodSymParam(ms, pos)) return p->hasDefault;
    return false;
}

bool formalParamHasDefault(const ConstructorSymbol& cs, std::size_t pos) {
    if (const ast::Param* p = ctorDeclParam(cs, pos))
        return p->defaultValue != nullptr;
    if (const ParameterSymbol* p = ctorSymParam(cs, pos)) return p->hasDefault;
    return false;
}

// IMPORT-only by construction: a local formal's default is the `ast::Expr` on
// its `ast::Param`, which every local consumer already lowers directly, so this
// deliberately returns "" for one rather than re-rendering it as text.
std::string_view formalParamDefaultText(const MethodSymbol& ms,
                                        std::size_t pos) {
    if (methodDeclParam(ms, pos)) return {};
    if (const ParameterSymbol* p = methodSymParam(ms, pos))
        return p->defaultText;
    return {};
}

std::string_view formalParamDefaultText(const ConstructorSymbol& cs,
                                        std::size_t pos) {
    if (ctorDeclParam(cs, pos)) return {};
    if (const ParameterSymbol* p = ctorSymParam(cs, pos)) return p->defaultText;
    return {};
}

// See Symbol.hpp. The admitted set is exactly what `renderConstExpr` emits for a
// LITERAL: an integer/float literal (possibly signed, and possibly the result of
// its literal-arithmetic fold), a quoted string, a char literal, `true`/`false`,
// or `null`. An identifier or `A.B` rendering starts with a letter or `_` and is
// deliberately excluded — it names something on the EXPORTER's side.
//
// fix #3213 (§22.5 / §2.5, v0.254): two more shapes are admitted here.
//
//   * `default(T)` — the textual rendering a `:= default(T)` default-value
//     expression carries. The importer materializes it as the zero value of the
//     FORMAL PARAMETER's resolved type (`MetaParam::type`, on the wire since
//     the format's REC_SCALARS record), so the literal T spelling in the text
//     is decorative — its only role is the "default(" prefix that identifies
//     this shape to the importer. Mirrors the local-twin codegen path
//     (LlvmBackend_Assign.cpp:10867), which lowers `default(T)` via
//     `Constant::getNullValue(lowerType(p.type))`.
//
//   * `:= C.K` / `:= SomeConst` const refs — FOLDED by `renderConstExpr` to
//     their constant value before this predicate ever sees them, so the
//     resulting text is a literal that is already admitted by the rules below.
//     No change to the predicate's recognized literal shapes — only the
//     "FOLD, don't render" change to the producer, in `renderConstExpr`.
bool importedDefaultTextIsMaterializable(std::string_view text) {
    if (text.empty()) return false;
    if (text == "true" || text == "false" || text == "null") return true;
    // fix #3213: the `default(T)` shape. Admitted by prefix so the importer
    // can recognize the family without a separate wire bit; the actual
    // materialization is from the formal's own type, not the text's T.
    if (text.size() >= 8 && text.compare(0, 8, "default(") == 0) return true;
    const char c = text.front();
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '"' ||
           c == '\'';
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

// §9.8 / §18.5 / §21.4 / §14.2 (#3139, family F1 of #3152): see Symbol.hpp for
// the contract. Modelled on `builtinExceptionBaseOf` directly above — a
// TypeSymbol field, a MetaTypeDecl record, and ONE walk that never requires an
// `ast::TypeDecl` at any level, because an imported type never has one.
const std::vector<TypeRef*>& nonSymbolBaseRefsOf(const TypeSymbol* t) {
    static const std::vector<TypeRef*> kEmpty;
    return t ? t->nonSymbolBaseRefs : kEmpty;
}

bool declaresBuiltinInterface(const TypeSymbol* t, std::string_view name) {
    if (!t || name.empty()) return false;
    // Source 1 — the built-in interfaces with NO TypeSymbol (ISerializable,
    // IDeserializable, IFormattable, IEnumerable<T>, …). Local: recorded by
    // Type Resolution; imported: restored by ModuleLoader from the metadata.
    // `headName()` is the alias-transparent head and reads `importedName` on
    // the imported side, so one spelling covers both.
    for (const TypeRef* b : t->nonSymbolBaseRefs)
        if (b && b->headName() == name) return true;
    // Source 2 — the built-in interfaces that DO get a synthesized TypeSymbol
    // (IComparable, IEquatable, … — SemaContext::isSynthesizedBuiltinInterface
    // Name) and so ride `baseTypes` on both sides already. The flag, not the
    // name alone, is the test: a USER interface named `IComparable` is a
    // different type and must not answer yes (§2.8.3 rule 3 identity).
    for (const TypeSymbol* b : t->baseTypes)
        if (b && b->isSynthesizedBuiltinInterface && b->name == name)
            return true;
    return false;
}

bool inheritsBuiltinInterface(const TypeSymbol* t, std::string_view name) {
    if (!t || name.empty()) return false;
    // Cycle-guarded BFS over the USER-base closure. An inheritance cycle is
    // reported by the §12 check but can still survive error recovery into this
    // walk, and the walk must terminate on one.
    std::vector<const TypeSymbol*> queue{t};
    std::unordered_set<const TypeSymbol*> seen{t};
    for (std::size_t qi = 0; qi < queue.size(); ++qi) {
        const TypeSymbol* cur = queue[qi];
        if (!cur) continue;
        if (declaresBuiltinInterface(cur, name)) return true;
        for (const TypeSymbol* b : cur->baseTypes)
            if (b && seen.insert(b).second) queue.push_back(b);
    }
    return false;
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

// §9.2 / §22.9 / §12.5: see Symbol.hpp. The AST-ONLY half — a non-static,
// non-indexer property whose `ast::PropertyDecl` declares the auto form
// (`Name: T;` — no expression body, no accessor block). Answers `false` for
// every IMPORTED property, because `ps->decl` is null there; that is why the
// storage question is asked through `isAutoPropertyStorageSymbol` below and
// not through this.
//
// fix #2939 (§12.5/§23.1/§9.2): an `abstract` simple-form property
// (`public abstract property X: int;`) is spelled identically to the auto
// form and so records `bodyKind == Auto`, but §12.5, verbatim: "`abstract` on
// a member declares it without a body; every concrete subclass must override
// it." §23.1's "The compiler generates the backing field" presupposes a body,
// which an abstract member has by construction not — so an abstract
// simple-form property has no backing field to generate and the storage
// classification MUST answer `false`. The pre-fix predicate answered `true`,
// so `collectInstanceStorage` reserved a phantom storage slot every derived
// class inherits by prefix layout (the issue's measured 4-byte waste per
// abstract simple-form property, never written or read). This is the AST-
// predicate half; the wire-bit half (`MetaMember::isAutoProperty`, set at
// `module/ModuleMetadataBuilder.cpp`'s export of the same shape) carries the
// same exclusion so an IMPORTED abstract simple-form property reaches the
// same answer through `isAutoPropertyStorageSymbol`'s decl-null fallback.
// Note: `isAutoPropertyStorageSymbol` is also the disposal-classification
// gate (the `sourceIsNamedOwner`/`srcIsNamedOwner` consumers in
// `lowerIEnumerableBuiltinFor` and the using/await paths, #2485). Flipping
// the abstract case from "named borrow" to "freshly produced" is the same
// direction the interface-property exclusion would go if `storageMemberOf`
// did not guard it (#2931); the #2939 path has no live wrong-value bug
// because the simple-form abstract property is implicitly get-only (§12.5,
// §23.1's "compiler generates the backing field" — no setter on an Auto
// bodyKind either way) and every read dispatches through `lowerVirtualPropertyGet`
// before reaching `fieldAddr`/`lvalueAddr`. The disposal consequence is
// "dispose a fresh value" — correct for any override that returns a freshly
// produced result, conservative-but-not-wrong for an auto-property override
// (whose value is still owned by its own backing field, never by the
// iteration source).
// §12.5 (found fixing #4012; #4018/#4020): see Symbol.hpp. Pure AST-level
// test — no Symbol/SemaContext needed — so it also serves callers (e.g.
// DefiniteAssignment.cpp's `isSevenNineStrictAutoProp`, Ownership.cpp's
// `isNonNullableReferenceAutoProp`, TypeResolution_Checks.cpp's per-type BFS)
// that walk `klass.members` directly and have not (yet, or ever) resolved a
// PropertySymbol for the member they are looking at.
// NOTE (#4037, #4100): the prior `p.isIndexer` guard excluded ALL indexers.
// That is correct for the storage-classification call sites (indexers never
// get a backing field regardless of abstract-ness), but WRONG for the setter-
// axis question: an abstract auto-indexer (`abstract property [i: int]: T;`)
// IS a §12.5 shorthand (get-only, no setter) and must return true here so
// that callers on the setter axis (e.g. `propertyDeclHasSetter`) do not
// incorrectly report a setter as present. The guard is removed so that
// abstract auto-indexers are treated identically to abstract auto-properties.
bool isAbstractPropertyShorthand(const ast::PropertyDecl& p) {
    if (p.bodyKind != ast::PropertyBodyKind::Auto) return false;
    for (auto mod : p.modifiers)
        if (mod == "abstract") return true;
    return false;
}

bool hasAstAutoPropertyDecl(const Symbol* m) {
    auto* ps = dynamic_cast<const PropertySymbol*>(m);
    if (!ps || ps->isIndexer) return false;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd || pd->isIndexer || pd->bodyKind != ast::PropertyBodyKind::Auto)
        return false;
    for (auto mod : ps->modifiers)
        if (mod == "static") return false;
    if (isAbstractPropertyShorthand(*pd)) return false;
    return true;
}

// §9.2 / §22.9 / §2.8.2 clause 1 (umbrella #3152): see Symbol.hpp. The one
// storage predicate — the declaration when the symbol has one, the #2478 wire
// bit when it does not. Consulting both is the point: switching on which source
// EXISTS is how the same question came to have two answers on the two sides of
// a module boundary. codegen calls this directly; there is no `Emitter` copy.
bool isAutoPropertyStorageSymbol(const Symbol* m, const SemaContext& ctx) {
    if (hasAstAutoPropertyDecl(m)) return true;
    auto* ps = dynamic_cast<const PropertySymbol*>(m);
    if (!ps || ps->isIndexer) return false;
    // A property that HAS a declaration was already answered above; only a
    // decl-less (imported) one falls through to the wire bit.
    if (ps->decl != nullptr) return false;
    return ctx.importedPropertyIsAutoStorage(ps);
}

// §9.1 / §9.2 / §22.8 (v0.254, fix #3182): see the contract in sema/Symbol.hpp.
//
// The three arms in order, and what each one used to be:
//   * `static` -> not instance storage. Was read off `ast::FieldDecl::modifiers`;
//     `FieldSymbol::modifiers` IS that same vector locally
//     (SymbolCollection.cpp copies it) and is filled from the wire on import, so
//     the test is unchanged for a local field and now answers for an imported
//     one. A §9.1 positional field carries NO modifiers on either side and is
//     never static, so it falls through this arm exactly as before.
//   * a positional field -> storage iff the primary-ctor parameter was captured
//     (#2325's `posParamBodyRef`). Unchanged.
//   * anything else -> instance storage. This is the arm that changed: it used
//     to require an `ast::FieldDecl`, which is null for every import.
//
// fix #3221 (§22.10/§9.8): the positional arm's `posParamBodyRef` now crosses
// the wire (MetaMember::posParamBodyRef, the 18th trailing scalar, additive
// within v7 — same shape as isPositional), so an imported class's captured
// primary-ctor parameter answers TRUE here when its exporter captured it
// (i.e. referenced the parameter in a body position per §22.10). Locally
// nothing changes — the `posParamBodyRef` ExprTypecheck sets has always been
// the answer this arm asks — and an imported record's positional field is
// unchanged because records' positional fields have no body references.
bool isInstanceFieldStorageSymbol(const Symbol* m) {
    const auto* fs = dynamic_cast<const FieldSymbol*>(m);
    if (!fs) return false;
    for (std::string_view mod : fs->modifiers)
        if (mod == "static") return false;
    if (fs->posParam) return fs->posParamBodyRef;
    return true;
}

// §22.15 (v0.286, fix #3609): see the contract in sema/Symbol.hpp. Symbol-side
// first, AST second, modifier third — the order is immaterial (nothing sets two
// of them to different answers) but the SET matters: each is the only populated
// source on some path.
//
// This is #3174's `constructorIsPure` generalised off `ConstructorSymbol`. The
// only structural change is that BOTH decl arms are tried, since a
// `MemberSymbol` may be either kind; a `MethodSymbol` never carries an
// `ast::ConstructorDecl` and a `ConstructorSymbol` never an
// `ast::OperatorDecl`, so neither arm can fire on a symbol the other arm
// serves, and no call site's answer moves.
bool memberIsPure(const MemberSymbol* m) {
    if (!m) return false;
    if (m->isPure) return true;
    // The AST arms. Both are *supposed* to answer null for an import — that is
    // what the `m->isPure` arm above is for — so they are sanctioned decl
    // reads, not unconverted ones.
    // SEAM-ACCESSOR: the operator arm of the both-sources purity predicate.
    if (auto* od = dynamic_cast<const ast::OperatorDecl*>(m->decl))
        if (od->isPure) return true;
    // SEAM-ACCESSOR: the constructor arm of the both-sources purity predicate.
    if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(m->decl))
        if (cd->isPure) return true;
    for (std::string_view mod : m->modifiers)
        if (mod == "pure") return true;
    return false;
}

// §22.15 (v0.254, fix #3174; generalised by #3609): the constructor-typed
// alias. Kept as its own name because #3174's recorded contract, its call
// sites, and the chain-purity comment in PureChecker.cpp all name it.
bool constructorIsPure(const ConstructorSymbol* cs) {
    return memberIsPure(cs);
}

// §22.15 "Purity attaches to the getter" (v0.286, fix #3609; fix #3618).
// See the contract in sema/Symbol.hpp. Strictly wider than `memberIsPure` —
// the accessor-level `pure get` marker is a channel the property's modifier
// list does not carry.
bool propertyGetterIsPure(const PropertySymbol* ps) {
    if (!ps) return false;
    if (memberIsPure(ps)) return true;
    // fix #3618: `PropertySymbol::accessorIsPure` is the symbol-side
    // counterpart to the wire's `MetaMember::accessorIsPure` (#3618's
    // 19th scalar) AND the same field populated locally by
    // SymbolCollection's PropertyDecl arm from `pd->accessors` — same
    // answer on both sides of the module boundary, exactly the seam the
    // original predicate left open (see the SEAM-ACCESSOR comment that
    // used to live below). For an imported property the AST walk below
    // would have answered "no" (`ps->decl == nullptr`), the silent-wrong-
    // classification direction the bug measured; for a LOCAL property
    // it agrees with the AST walk but skips a redundant `dynamic_cast`.
    // SEAM-ACCESSOR: the accessor-level arm of the getter-purity predicate.
    // Both sides of the module boundary populate this slot from the same
    // source (`pd->accessors` locally in SymbolCollection's PropertyDecl
    // arm; `MetaMember::accessorIsPure` on the wire in ModuleLoader's
    // PropertySymbol / Indexer arms, #3618's 19th scalar), so the
    // dynamic_cast-based AST walk that used to live below would have
    // answered "no" for every imported symbol — the silent-wrong-
    // classification direction #3618 fixed — and is now removed. A
    // future code path that constructs a PropertySymbol without setting
    // `accessorIsPure` will fail loudly (this predicate returns false,
    // §22.15 rule 6 rejects the read at the impure call site) rather
    // than silently agree with an AST walk that #3618 already proved
    // unsound across the seam.
    if (ps->accessorIsPure) return true;
    // fix #3630 narrow (§22.15 v0.286, over-reject follow-up): an
    // auto-property's getter is the synthesized `return backing-field`
    // shape, which is pure-by-construction. Without this arm, a pure body
    // reading an auto-property (the overwhelmingly common record shape
    // `record R { public readonly property X: int; }`) is over-rejected by
    // #3630's MemberAccessExpr arm — the regression measured on
    // xmod_pure_ctor_chain_3174.t5 and xmod_record_sibling_ctor_3219.t1b.
    // `ps->isAutoProperty` is set in the LOCAL SymbolCollection PropertyDecl
    // arm from `hasAstAutoPropertyDecl(sym)` and in the IMPORTED ModuleLoader
    // Property/Indexer arms from `MetaMember::isAutoProperty` (#2478's 13th
    // wire scalar), so the predicate is identical on both sides of the
    // module boundary.
    if (ps->isAutoProperty) return true;
    return false;
}

// §9.2 (v0.276, fix #3326): the §9.2/§25.2 homogeneity test shared by both
// record-eligibility walks. A `=`/`<>` operator overload is **homogeneous**
// iff BOTH of its operands resolve to the declaring record/enum/choice type —
// the third §9.2 disqualifier and the §25.2 co-declaration trigger both
// key on this single fact. A **heterogeneous**-operand overload (`T = U`,
// `T = int`, …) defines a comparison the synthesis never provided and takes
// over nothing — §9.2 keeps the record auto-hashable, and §25.2 carries no
// co-declaration obligation. Resolver::checkEqualityCoDeclaration
// (TypeResolution_Checks.cpp:2274-2285) has the SAME predicate inlined for
// its first-match scan; that site is left alone (its logic is slightly
// different — it needs the *first* homogeneous overload's declLoc for
// diagnostics, not just yes/no — and refactoring it is out of scope per the
// fix's "reusing the existing helper" guidance: one helper for the two NEW
// uses, the existing call site keeps its inline copy).
static bool isHomogeneousEqualityOperatorOverload(const MethodSymbol* meth,
                                                  const TypeSymbol* rec,
                                                  const SemaContext& ctx) {
    if (!meth || !rec) return false;
    if (!meth->isOperator) return false;
    if (meth->operatorToken != "=" && meth->operatorToken != "<>") return false;
    if (meth->params.size() != 2) return false;
    // Homogeneous iff BOTH operands resolve to the declaring type.
    for (Symbol* psym : meth->params) {
        auto* p = dynamic_cast<ParameterSymbol*>(psym);
        const TypeRef* pt = (p && p->declType)
            ? ctx.resolvedType(*p->declType) : nullptr;
        if (!pt || pt->kind != TypeRef::Kind::Named ||
            pt->declaration != rec) { return false; }
    }
    return true;
}

// §9.3 (v0.158, #1383): forward declaration for mutual recursion with
// isAdmissibleComponentType — a positional field's type calls
// isAdmissibleComponentType recursively, which in turn calls this function
// to check whether a nested positional record is itself hashable.
// Definition follows isAutoEqualityBearingBodyRecord below.
bool isPositionalRecordEqualityBearing(const TypeSymbol* rec,
                                        SemaContext& ctx);

// §35.7 (v0.210, #2083): declared in Symbol.hpp (with the #2047 `inst`
// instantiation parameter). Body is below — after the admissibility predicate
// it depends on (`isAdmissibleComponentType` calls it).

// §9.2 v0.158: resolve a record storage component (an auto-property, a body
// field, or a §9.1 positional field) to its declared TypeRef. Returns null when
// the type is not resolvable (an imported member with no local ast::Type node
// and no side-table entry) — the caller then treats the component conservatively
// as admissible.
//
// fix #3145: no longer `static`. It is declared in sema/Symbol.hpp because it is
// the ONE accessor that consults both sources, and being file-local is exactly
// why two more copies grew elsewhere (Sendability.cpp's lambda, and
// OwnershipFacts.cpp's raw AST walk, which simply saw nothing for an import).
const TypeRef* storageComponentType(const Symbol* m,
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

// fix #3169 (§9.1 / §9.2 / §30.14 rule 2, v0.242; refactor of #3145/#3152):
// the unified symbol-side storage enumeration. See the contract in
// sema/Symbol.hpp. This is the ONE storage walk; the three overlapping
// accessors (`anyRecordStorageField` below, `recordPositionalFieldSymbols`
// further down, `forEachStorageMember` in Sendability.cpp) each delegate to
// it. Per-member FACTS are computed once — including the §28.1 `owned` marker
// from BOTH `ast::Param::modifier` (positional) and `MemberSymbol::modifiers`
// (body / auto-prop), per #3145's both-source rule.
//
// `static` is excluded because a static field is per-TYPE storage, not part of
// a VALUE — the same exclusion Resolver::recordValueContainsSelf and the AST
// walk this replaces both make. §30.14 rule 2 is "every INSTANCE field,
// including inherited fields", so a static never travels with a transferred
// instance and is not yielded.
//
// For a CLASS parent's primary-ctor parameter, the capture bit
// `FieldSymbol::posParamBodyRef` (#3165/#3221) decides: only a CAPTURED
// parameter (body-referenced) is storage; an uncaptured one is skipped
// entirely. For a RECORD, every §9.1 positional field is storage — the
// `recordPositionalIsStorage` guard at the outer loop expresses that.
//
// ORDER: positional pass first, then non-positional pass — matching the order
// the AST walks the umbrella #3152 collapse chose, so a §14.2 component-path
// diagnostic names a positional field before a body one. Only the diagnostic
// walks can observe the order (verdict walks require every member to pass),
// but it pins which component a §14.2 message names.
bool recordStorageMembers(
    const TypeSymbol* ts, const SemaContext& ctx,
    const std::function<bool(const RecordStorageMember&)>& fn,
    const common::RecordSubstEnv* env) {
    if (!ts) return true;
    const bool recordPositionalIsStorage =
        ts->kind == ast::TypeDeclKind::Record;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0 && !recordPositionalIsStorage) {
            // For a class, fall through: the inner filter decides per field.
            // For a non-class non-record, skip pass 0 — there are no §9.1
            // positional fields in either kind today.
            if (ts->kind != ast::TypeDeclKind::Class) continue;
        }
        for (Symbol* ms : ts->members) {
            const MemberSymbol* mem = nullptr;
            const ast::Param* posParam = nullptr;
            RecordStorageMember rsm;
            rsm.member = ms;
            if (auto* fs = dynamic_cast<const FieldSymbol*>(ms)) {
                const bool positional = fs->posParam != nullptr;
                if (positional != (pass == 0)) continue;
                // For a CLASS parent (fix #3165): a primary-ctor parameter is
                // storage iff §22.10 captures it (body-referenced). An
                // uncaptured one has no field on the instance and rule 2 has
                // nothing to weigh. For records, every positional is storage.
                if (positional && ts->kind == ast::TypeDeclKind::Class &&
                    !fs->posParamBodyRef) {
                    continue;
                }
                bool isStatic = false;
                for (std::string_view mod : fs->modifiers)
                    if (mod == "static") { isStatic = true; break; }
                if (isStatic) continue;
                mem = fs;
                posParam = fs->posParam;
                rsm.name = (posParam && !posParam->name.empty())
                               ? posParam->name
                               : fs->name;
            } else if (auto* ps = dynamic_cast<const PropertySymbol*>(ms)) {
                if (pass == 0) continue;  // properties are never positional
                if (!isAutoPropertyStorageSymbol(ps, ctx)) continue;
                bool isStatic = false;
                for (std::string_view mod : ps->modifiers)
                    if (mod == "static") { isStatic = true; break; }
                if (isStatic) continue;
                mem = ps;
                rsm.isAutoProperty = true;
                rsm.name = ps->name;
            } else {
                continue;  // methods/ctors/consts/events/nested: not storage
            }
            rsm.type = storageComponentType(ms, ctx);
            // #3171: weigh a generic base's fields as the SUBSTITUTED type,
            // not the bare declaration-template parameter.
            if (env) {
                rsm.type = common::resolveRecordFieldSubst(rsm.type, env).ty;
            }
            // The `owned` marker lives in two DIFFERENT places — bc0edd5's
            // warning, earned by the prior `anyRecordStorageField` walker.
            // `MemberDecl::modifiers` for a body field or auto-property,
            // `ast::Param::modifier` for a positional one. Read both.
            if (posParam) {
                rsm.isOwned = posParam->modifier == "owned";
                if (!rsm.isOwned) {
                    for (std::string_view mod : mem->modifiers)
                        if (mod == "owned") { rsm.isOwned = true; break; }
                }
            } else {
                for (std::string_view mod : mem->modifiers)
                    if (mod == "owned") { rsm.isOwned = true; break; }
            }
            if (!fn(rsm)) return false;
        }
    }
    return true;
}

// fix #3145 (§9.1 / §9.2 / §2.8.3 rule 4, v0.252) — refactor #3169: the
// fields-only existential. Delegates to `recordStorageMembers` (the unified
// primitive); auto-properties are skipped, only FieldSymbols are visited.
//
// `static` is excluded because a static field is per-TYPE storage, not part of
// a VALUE — `recordStorageMembers` already filters it. Positional fields
// carry no modifiers and are never static, so one filtered loop is exactly
// equivalent to the old "positionalFields, then non-static body FieldDecls"
// pair. ⚠ ORDER IS `members` ORDER, WHICH IS NOT DECLARATION ORDER for a
// combined-form record: this predicate is an existential, so order cannot
// change its answer — but a future caller that needs order must not take it
// from here. Use `recordPositionalFieldSymbols` for the index-aligned
// positional view.
bool anyRecordStorageField(
    const TypeSymbol* ts, const SemaContext& ctx,
    const std::function<bool(const Symbol*, const TypeRef*)>& pred) {
    if (!ts) return false;
    bool result = false;
    recordStorageMembers(
        ts, ctx,
        [&](const RecordStorageMember& rsm) {
            if (rsm.isAutoProperty) return true;  // fields-only existential
            if (pred(rsm.member, rsm.type)) {
                result = true;
                return false;  // stop
            }
            return true;
        });
    return result;
}

// fix #3140 (§9.8, family F2 of umbrella #3152): the ONE reader of a storage
// member's `serialized` clause. Every field it consults is populated on BOTH
// sides — SymbolCollection from `ast::Param::serialized` /
// `ast::PropertyDecl::serialized`, ModuleLoader from `MetaMember` — so this
// answers for an imported member without ever touching `Symbol::decl`, which is
// null for every one of them.
//
// The wire-name default is folded in deliberately. §9.8's rule is "the wire name
// is the `serialized "<name>"` rename when present, otherwise the declared
// identifier", and that sentence was written out separately at five sites in
// LlvmBackend_Serialization.cpp (twice for positional fields, twice for
// auto-properties, once for choice cases). Returning the resolved name is what
// stops a sixth copy.
SerializedComponentClause serializedComponentClause(const Symbol* m) {
    SerializedComponentClause info;
    if (!m) return info;
    info.wireName = m->name;
    const auto* ms = dynamic_cast<const MemberSymbol*>(m);
    if (!ms) return info;
    info.skip     = ms->serializedSkip;
    info.optional = ms->serializedOptional;
    if (!ms->serializedWireName.empty()) info.wireName = ms->serializedWireName;
    return info;
}

// §9.2 "The `equality skip` clause" (v0.307; follow-up to fix #4643): see the
// declaration comment in Symbol.hpp for the measured cross-module divergence
// this closes — a positional record comparing `True` in its own module and
// `False` across the import, and a body record whose `=` is accepted locally
// and rejected at the import.
bool memberEqualitySkip(const Symbol* m) {
    const auto* ms = dynamic_cast<const MemberSymbol*>(m);
    return ms && ms->equalitySkip;
}

// fix #3140 (§9.8, family F2): see the declaration comment in Symbol.hpp for
// why an imported class answers "" and what was measured when it did not.
std::string_view serializedClassShape(const TypeSymbol* t) {
    if (!t) return {};
    return t->serializedShape;
}

const std::vector<SerializedPolyCase>& serializedPolyCasesOf(
        const TypeSymbol* t) {
    static const std::vector<SerializedPolyCase> kEmpty;
    return t ? t->serializedPolyCases : kEmpty;
}

// fix #3140 follow-up (§9.8): see the declaration comment in Symbol.hpp for the
// measured 22-vs-50 byte divergence this closes and for why the list is
// "carriage is incomplete", not "§9.8 admits".
bool serializationComponentTypeIsEmittable(const TypeRef* ty) {
    if (!ty) return true;  // unresolved: another check owns it; do not add a
                           // second opinion here.
    if (ty->kind == TypeRef::Kind::InlineArray) {
        if (!ty->arrayElement) return false;
        // EXACTLY the condition `Emitter::lowerTypeRef` uses before it gives up
        // and returns the bare `i32` placeholder — stated once, here, so the two
        // cannot drift: a size that is neither an AST expression (local) nor a
        // folded value (#734's member-const form) did not survive to codegen.
        // `ModuleMetadataBuilder` never writes one, so this is false for every
        // IMPORTED inline array and true for every local one.
        if (!ty->arraySize && ty->arraySizeValue < 0) return false;
        return serializationComponentTypeIsEmittable(ty->arrayElement);
    }
    return true;
}

namespace {
// The recursive half of `serializationSurfaceIsEmittable`. `depth` bounds a
// pathological nesting chain; the `seen` set bounds a legitimate cycle (§9.8
// admits a record component whose type is reached again through another
// record, and the eligibility check treats a cycle as eligible).
bool surfaceEmittableImpl(const TypeSymbol* t, const SemaContext& ctx,
                          std::unordered_set<const TypeSymbol*>& seen,
                          int depth) {
    if (!t || depth > 16) return true;   // conservative, matching the
                                         // eligibility walk's own posture
    if (!seen.insert(t).second) return true;  // cycle: already being checked
    // fix #3185 (§9.8 / §35, v0.252; family F2 of umbrella #3152): the
    // IMPORTED-CHOICE interlock that lived here was the *symptom side* of the
    // same defect the record/enum/choice emitters had. Before this fix it
    // returned false for any `kind == Choice && ctx.isImported(t)`, which kept
    // the surface from being admitted at all; the #3139 blanket interlock was
    // already gone, and the actual codegen defect (`!ts->decl` early-return in
    // emitChoiceSerialize / emitChoiceDeserialize — see
    // LlvmBackend_Serialization.cpp:1851/:1934) had been fixed by removing that
    // guard. With the emitters now walking the reconstructed `caseDecl`
    // (F9 #3144 carries name, payload field names, the §28.1 `owned` marker,
    // each payload's resolved type via `setResolvedType`, AND the §9.8
    // `serialized` clause on the case AND each payload field), the imported
    // choice path is sound on every consumer:
    //   * choiceCasesOf iterates `t->members` in declaration order — the
    //     tag order, which `ModuleLoader::convertChoiceCases` appends in
    //     `td.choiceCases` order so importer and exporter agree on tags;
    //   * choiceStruct lays out `{ i32 tag, case0_payload, … }` via
    //     `choiceCasePayloadStruct`, which reads each `pf.type` and calls
    //     `lowerType(pf.type.get())` — the synthetic payload node carries a
    //     published resolved entry;
    //   * choiceCaseWireName / positionalFieldWireName / positionalFieldIsSkip
    //     all walk `cc->caseDecl->serialized` / `pf.serialized`, which the
    //     reconstructor populates from `MetaChoiceCase::serialized*` /
    //     `MetaChoicePayloadField::serialized*` fields;
    //   * choiceHasStringFields (the disposal-side companion, see
    //     LlvmBackend_Disposal.cpp:2519) reads `cc->caseDecl->payload` only,
    //     and never `ts->decl`;
    //   * the per-case payload walk inside emitChoiceSerialize / deserialize
    //     resolves each payload's type via `ctx_.resolvedType(*pf.type)`, which
    //     hits the published resolved entry.
    //
    // MEASURED, post-fix, on the same two-module build the interlock was
    // measured against:
    //   direct `Status.Deserialize(d)` round-trip   IMPORTED 7, LOCAL 7
    //   local `record Holder(S: Status, N: int)`    IMPORTED 8, LOCAL 8
    //   case `serialized "hh"` rename oracle        the LOCAL twin with the
    //                                               "hh" rename reads 7
    //                                               (the local twin without it
    //                                                raises SerializationException)
    //
    // `ctx.isImported` was the right question (a `t->decl` read would have been
    // a dead-pointer shape the umbrella #3152 exists to remove); it is no
    // longer needed because the imported choice is now emittable.
    for (const Symbol* m : t->members) {
        const auto* fs = dynamic_cast<const FieldSymbol*>(m);
        const bool isField = fs != nullptr;
        const bool isAutoProp = isAutoPropertyStorageSymbol(m, ctx);
        if (!isField && !isAutoProp) continue;
        if (isField) {
            bool isStatic = false;
            for (std::string_view mod : fs->modifiers)
                if (mod == "static") { isStatic = true; break; }
            if (isStatic) continue;
        }
        // §9.8: a `serialized skip` component is off the wire entirely, so its
        // shape cannot make the surface unemittable.
        if (serializedComponentClause(m).skip) continue;
        const TypeRef* cty = storageComponentType(m, ctx);
        if (!serializationComponentTypeIsEmittable(cty)) return false;
        // Recurse into a nested record/choice component: §9.8's eligibility
        // check trusts such a component as "checked at its own conformance
        // site", and for an IMPORTED type that site never ran.
        if (cty && cty->kind == TypeRef::Kind::Named && cty->declaration &&
            (cty->declaration->kind == ast::TypeDeclKind::Record ||
             cty->declaration->kind == ast::TypeDeclKind::Choice)) {
            if (!surfaceEmittableImpl(cty->declaration, ctx, seen, depth + 1))
                return false;
        }
    }
    return true;
}
}  // namespace

bool serializationSurfaceIsEmittable(const TypeSymbol* t,
                                     const SemaContext& ctx) {
    std::unordered_set<const TypeSymbol*> seen;
    return surfaceEmittableImpl(t, ctx, seen, 0);
}

// fix #3145: the indexed view. `posParam != nullptr` is the positional marker on
// BOTH sides — a real `ast::Param` locally, a synthetic one on import (#2516) —
// and `members` preserves declaration order on both, so filtering keeps
// positional field i at index i.
// §35.1 (fix #3144, family F9 of #3152) — see the contract on the declaration
// in Symbol.hpp. Declaration order is the tag order; `members` preserves it on
// both sides (SymbolCollection appends cases last, ModuleLoader mirrors that).
std::vector<const ChoiceCaseSymbol*> choiceCasesOf(const TypeSymbol* t) {
    std::vector<const ChoiceCaseSymbol*> out;
    if (!t) return out;
    for (const Symbol* m : t->members)
        if (auto* cc = dynamic_cast<const ChoiceCaseSymbol*>(m))
            out.push_back(cc);
    return out;
}

std::vector<const Symbol*> recordPositionalFieldSymbols(const TypeSymbol* ts) {
    std::vector<const Symbol*> out;
    if (!ts) return out;
    for (Symbol* m : ts->members)
        if (auto* fs = dynamic_cast<const FieldSymbol*>(m))
            if (fs->posParam) out.push_back(m);
    return out;
}

// §9.2 v0.158 admissible-component-type predicate (one recursive rule, applied
// per storage component):
//   - a primitive value type, an enum, or `string`;
//   - another record that is itself auto-hashable (recursively);
//   - `T?` where `T` is admissible (the null-safe `=` of §8.4/§7.11);
//   - a non-owning inline array `[N]T`/`[N]T?` whose element is admissible
//     (element-by-element per §8.3's inline-array rows);
//   - a **value-like choice** that is itself auto-hashable
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
static bool isAdmissibleComponentType(const TypeRef* ty, SemaContext& ctx,
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
            if (!decl) {
                // fix #3327 (§9.2 / §25.2 v0.276): the compiler-intrinsic FFI
                // pointer records `IntPtr` / `UIntPtr` / `Ptr<T>` (§10.0/§10.5)
                // resolve to a Named TypeRef with no user-side TypeDecl, so the
                // null-declaration path above would otherwise admit them as
                // components. Each declares its own homogeneous `=`/`<>`
                // overload (§25.2's co-declaration rule, retired-exemption
                // v0.275), which v0.276's third disqualifier uses to strip
                // auto-hashable — and §9.2's recursive component rule reads
                // "another record that is itself auto-hashable", so an
                // enclosing record carrying one of these as a component is
                // likewise not auto-hashable. Reject explicitly; let every
                // *other* null-declaration case (unresolved generic
                // parameter, imported builtin) stay conservative-true so a
                // generic body / an imported record is never over-rejected.
                std::string_view h = ty->headName();
                if (h == "IntPtr" || h == "UIntPtr" || h == "Ptr")
                    return false;
                return true;
            }
            if (decl->kind == ast::TypeDeclKind::Enum) return true;
            if (decl->kind == ast::TypeDeclKind::Record) {
                // Another record — admissible iff it is itself
                // auto-hashable. §9.3 (v0.158, #1383): a positional or
                // combined-form record synthesizes equality only when every
                // positional field (and body member for combined form) is
                // admissible; the blind `return true` for all positional
                // records was the pre-#1383 bug (over-accept). Imported
                // positional records (no ast::TypeDecl) remain conservative.
                // v0.276: the spec reads "auto-hashable" here, NOT
                // "hashable"; widening to "hashable" is deferred (DECLINED.md
                // row 320) because it would smuggle reference equality
                // through user-equality records carrying a class-typed field.
                if (recordIsPositional(decl))
                    return isPositionalRecordEqualityBearing(decl, ctx);
                return isAutoEqualityBearingBodyRecord(decl, ctx);
            }
            // §9.2 v0.210 (#2083): a CHOICE component is admissible iff it is
            // value-like AND itself auto-hashable (§35.7). Borrow-bearing
            // and owning choices are NOT admissible in any component — their
            // class-typed payload slots are reference or move-only types, exactly
            // the categories the rule rejects. Auto-hashable for choices
            // is the same predicate used by isEqualityBearing as a hash-key
            // check (and codegen uses it to dispatch the choice-specific
            // Equals/GetHashCode synthesis) — see isAutoEqualityBearingChoice.
            //
            // fix #3933 (§35.11/§35.4/§9.2, v0.295): `ty` (this call's OWN
            // parameter — already the substituted, fully-instantiated
            // component type, e.g. `substitutedChoicePayloadType`'s result)
            // was being discarded, so a nested choice component was always
            // judged at its BARE, uninstantiated declaration — an
            // uninstantiated generic choice's own bare-parameter payload
            // conservatively classifies borrow-bearing (§9.7's documented
            // "might carry a borrow" posture), which made `Pair` inadmissible
            // as a component NO MATTER what it was instantiated at, falsely
            // rejecting `Pair<Pair<int>>.Equals`/`=` (every payload `int`) as
            // well as correctly-but-differently-reasoned `Pair<Pair<Box>>`.
            // Threading `ty` mirrors the Record arm just above (which needs no
            // such thread — records carry no `owned` markers, §9.7 — but
            // §28.1's per-instantiation `owned`-marker erasure means a choice's
            // admissibility genuinely depends on the instantiation, exactly as
            // `checkEqualityMemberUseSite`'s own top-level call already does).
            if (decl->kind == ast::TypeDeclKind::Choice)
                return isAutoEqualityBearingChoice(decl, ctx, ty);
            // class / interface — reference types are not admissible.
            return false;
        }
    }
    return true;
}

// Declared in Symbol.hpp — see the doc comment there for the both-sources
// contract (#3600).
bool recordIsPositional(const TypeSymbol* rec) {
    if (!rec) return false;
    if (rec->hasPositionalFields) return true;
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(rec->decl))
        return td->hasPositionalFields;
    return false;
}

// §9.9 (v0.307, proposal 0107, fix #4699): see Symbol.hpp for the full
// reading. Local-only check: walk the AST `TypeDecl::members` for any
// `ConstructorDecl` whose `contractClauses` is non-empty. The local AST is
// always available for a record actually declared in the current module
// (`rec->decl` is the `ast::TypeDecl*`); the null-decl fallback is the
// cross-module (imported) case, for which this returns false conservatively
// (the wire-channel carriage is not yet built — see Symbol.hpp's
// IMPORTED READING).
//
// fix #4699 (cycle 23): landed as the predicate behind the three
// downstream checks the corpus targets — `default(R)` at §17.11,
// `where T: new()` substitution at §14.2, and default-initialized `R`-typed
// storage at §7.9.
bool recordIsConstructorOnly(const TypeSymbol* rec) {
    if (!rec) return false;
    if (rec->kind != ast::TypeDeclKind::Record) return false;
    // A positional record may not declare a clause-bearing constructor at
    // all (the declaration-site rule from fix #4702 in TypeResolution.cpp's
    // `checkRecordCombinedFormContractClauseConstructors`), so it cannot
    // be constructor-only. Both sources are consulted for symmetry with
    // `recordIsPositional` above.
    if (recordIsPositional(rec)) return false;
    // SEAM-DEBT: #3301 this is NOT a benign null branch. `recordIsConstructorOnly`
    // feeds USE-site consumers (§17.11 `default(T)`, §14.2 `new` constraints,
    // §7.9), so answering `false` for an IMPORTED record wrongly ADMITS
    // `default(ImportedCtorOnlyRecord)`. The §2.8.2 wire channel that would
    // carry the property is unbuilt — #3301 tracks it, and this file's own
    // IMPORTED READING in Symbol.hpp states the conservative fallback. Closing
    // #3301 must revisit this read; do not treat the early `false` as correct.
    auto* rawRecDecl = rec->decl;
    auto* td = dynamic_cast<const ast::TypeDecl*>(rawRecDecl);
    if (!td) return false;  // imported — see IMPORTED READING in Symbol.hpp
    for (const auto& m : td->members) {
        if (!m) continue;
        if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(m.get())) {
            if (!cd->contractClauses.empty()) return true;
        }
    }
    return false;
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
                                     SemaContext& ctx) {
    if (!rec || rec->kind != ast::TypeDeclKind::Record) return false;
    // §9.3: a record with positional fields is positional / combined form; it
    // gets the §9.1 positional synthesis, not this body-record path.
    if (recordIsPositional(rec)) return false;

    for (const Symbol* m : rec->members) {
        // An auto-property is the participating storage — eligible only when its
        // declared type is an admissible component type (§9.2 v0.158).
        //
        // fix #3152 (§9.2/§2.8.2 clause 1): the STORAGE predicate. This is the
        // "correct predicate fed by an empty source" shape — with the AST-only
        // one, an IMPORTED body record's auto-properties all fell through to
        // the `dynamic_cast<PropertySymbol*>` arm below, which reads them as
        // computed and non-disqualifying, so this function checked NO
        // components and returned `true`. A body record whose auto-property is
        // class-typed then got `=` across a module boundary that §9.2 rejects
        // in a single module.
        //
        // §9.2 (v0.307) / fix #4643: an auto-property marked `equality skip`
        // is removed from the auto-equality membership computation before the
        // admissibility check — that is the clause's rescue purpose for
        // otherwise-inadmissible auto-property storage (e.g. a class-typed
        // cache field whose address identity must not enter auto-`=`).
        if (isAutoPropertyStorageSymbol(m, ctx)) {
            if (memberEqualitySkip(m)) continue;
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
            //
            // §9.2 (v0.307) / fix #4643: a non-static field marked
            // `equality skip` is also removed from the auto-equality
            // membership computation — that is precisely the clause's purpose
            // (it rescues otherwise-ineligible auto-properties / fields by
            // declaring they are not part of the equality synthesis). The
            // static-field check above still rejects such a field as an
            // inert annotation; the (mutable) skip arm is the rescue path.
            bool isStatic = false;
            for (auto mod : fs->modifiers)
                if (mod == "static") { isStatic = true; break; }
            if (isStatic) continue;
            if (memberEqualitySkip(m)) continue;
            return false;
        }
        if (auto* meth = dynamic_cast<const MethodSymbol*>(m)) {
            // A manually-declared equality member suppresses the auto package
            // (§9.2): an `Equals`/`GetHashCode` method, or a *homogeneous*
            // `=`/`<>` operator overload (§9.2 v0.276, fix #3326). A
            // heterogeneous-operand `=`/`<>` overload (`T = U`) defines a
            // comparison the synthesis never provided and takes over
            // nothing — neither qualifies nor disqualifies, exactly as
            // §25.2 frees it from the co-declaration obligation for the
            // same reason.
            if (meth->isOperator) {
                if (isHomogeneousEqualityOperatorOverload(meth, rec, ctx))
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
// are conservatively treated as hashable for the positional-field
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
                                        SemaContext& ctx) {
    if (!rec) return false;
    for (const Symbol* m : rec->members) {
        if (auto* fs = dynamic_cast<const FieldSymbol*>(m)) {
            if (fs->posParam) {
                // §9.2 (v0.307) / fix #4643: a positional field marked
                // `equality skip` is removed from the auto-equality
                // membership computation. The clause is the rescue path for
                // an otherwise-inadmissible positional field — caching /
                // auxiliary storage that the auto-`=`/`<>` synthesis would
                // otherwise reject as a class-typed / move-only / reference
                // component. Checked BEFORE the admissibility test below so
                // the field's declared type is not consulted.
                if (memberEqualitySkip(m)) continue;
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
        // fix #3152: the STORAGE predicate — the combined-form twin of the
        // same empty-source defect in isAutoEqualityBearingBodyRecord above.
        if (isAutoPropertyStorageSymbol(m, ctx)) {
            if (!isAdmissibleComponentType(storageComponentType(m, ctx), ctx, 0))
                return false;
            continue;
        }
        // Computed (expression-bodied) property: no storage, does not disqualify.
        if (dynamic_cast<const PropertySymbol*>(m)) continue;
        if (auto* meth = dynamic_cast<const MethodSymbol*>(m)) {
            if (meth->isOperator) {
                // §9.2 v0.276 (fix #3326): only a HOMOGENEOUS `=`/`<>`
                // operator overload opts the record out of auto-equality —
                // a heterogeneous-operand overload (`T = U`) defines a
                // comparison the synthesis never provided and takes over
                // nothing. See isAutoEqualityBearingBodyRecord above for
                // the same predicate.
                if (isHomogeneousEqualityOperatorOverload(meth, rec, ctx))
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
// auto-hashable set declare `=` and `<>` overloads in the body, along
// with `Equals(Object?)`, `Equals(T)`, and `GetHashCode` overrides as needed."
// Only the operator overloads bypass the §9.2 compile error on `=`/`<>`: a
// record that declares `override Equals`/`override GetHashCode` WITHOUT the
// operators is NOT auto-hashable (those overrides are themselves
// disqualifiers per isAutoEqualityBearingBodyRecord above), and `=`/`<>` on
// it remains a compile error (#1249).  The `.Equals`/`.GetHashCode` call path
// is separately gated: lookupMember finds the user method and dispatches
// directly, so that path does not reach this function.
//
// §9.2 v0.276 (fix #3326, see isHomogeneousEqualityOperatorOverload above):
// for a heterogeneous-only overload (`T = U`) this function returns true but
// the record is ALSO auto-hashable (§9.2 v0.276 keeps it); the callers
// (isEqualityBearing, checkEqualityMemberUseSite) OR this predicate with the
// auto-hashable one, so the verdict is unchanged. The narrower "user took
// over equality" reading would require threading a SemaContext through this
// function's signature — out of scope per the fix's minimal-touch posture.
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

// §35.7 (v0.200, #2044) / v0.210 (#2083): a choice is auto-hashable
// iff (a) its §35.4 classification is ValueLike AND (b) every payload field of
// every case is a §9.2 admissible component type — with "another auto-
// hashable choice" admitted to the recursive rule. Per §35.7's first
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
// treated as auto-hashable: classifyChoiceType returns nullopt without
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
// §35.7 ruled like §9.2: they do not disqualify. The single body-member
// disqualifier is a homogeneous `=`/`<>` overload (§35.7, v0.276, #3333) —
// it strips the auto-hashable property exactly as it strips it on a record
// (§9.2 third-bullet, fix #3326). The rescue is §25.2's co-declaration rule,
// consulted via `choiceHasUserDeclaredEquality` from `isEqualityBearing` and
// the use-site checks in ExprTypecheck — a value-like choice that declares
// `=`/`<>` WITH the co-declared `Equals(T)`/`GetHashCode` is hashable, not
// auto-hashable.
//
// Pre-#3333, this predicate never scanned body members and treated every
// value-like choice with admissible payloads as auto-hashable — the
// "non-issue in practice" note from the previous revision. v0.276 widens the
// ruling to choices; this is that widening.
bool isAutoEqualityBearingChoice(const TypeSymbol* ch,
                                 SemaContext& ctx,
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
    // §35.7 (v0.276, #3333): a homogeneous `=`/`<>` overload on the choice
    // body disqualifies the choice from auto-hashable, mirroring §9.2's
    // record rule (fix #3326). The author has taken over equality; the
    // synthesized `=`/`<>`/`.Equals`/`.GetHashCode` are no longer present.
    // The §25.2 co-declaration rule supplies the equality surface — see
    // choiceHasUserDeclaredEquality below. Payload admissibility is not
    // consulted on this branch: the overload's presence is the disqualifier,
    // payload contents irrelevant (and a co-declaring author may have chosen
    // payloads on that basis). Walk body members, NOT case symbols — choice
    // cases are not member symbols, the overload rides on the choice body.
    for (const Symbol* m : ch->members) {
        if (auto* meth = dynamic_cast<const MethodSymbol*>(m)) {
            if (meth->isOperator &&
                isHomogeneousEqualityOperatorOverload(meth, ch, ctx))
                return false;
        }
    }
    // fix #3144 (family F9 of umbrella #3152): the payload walk is SYMBOL-side.
    // It read `dynamic_cast<const ast::TypeDecl*>(ch->decl)` and returned
    // `true` when that was null — which it is for EVERY imported choice — so an
    // imported value-like choice was declared auto-equality-bearing with its
    // payload types never examined once. That is an over-ACCEPT, and the audit
    // records the site as blocked on precisely this channel; with the cases on
    // the wire it is a real check.
    // Every case: every payload field, at THIS instantiation, must be a §9.2
    // admissible component type. #2047 / §28.1: an `owned` marker on a value-
    // like/runtime-managed payload (the only case that reaches here — the
    // classification gate above already rejected an owning instantiation) has
    // ERASED, so the payload's admissibility is decided by its instantiated
    // type like any component (a `string`/`int` payload is admissible). The
    // recursive `isAdmissibleComponentType` drives the body-record rule and
    // admits "another auto-hashable choice" via its K::Named/Choice
    // branch (v0.210, #2083).
    for (const ChoiceCaseSymbol* ccs : choiceCasesOf(ch)) {
        // fix #3144 (review nit N3): a case we cannot inspect must not be
        // skipped — skipping asserts "contributes no inadmissible payload"
        // about a case never examined. Not auto-equality-bearing is the safe
        // answer. // per §9.2 / §9.3
        if (!ccs->caseDecl) return false;
        for (const auto& pf : ccs->caseDecl->payload) {
            if (!pf.type) continue;  // unresolved → skip (defensive)
            const TypeRef* ft =
                common::substitutedChoicePayloadType(pf, ch, rt, ctx);
            if (!isAdmissibleComponentType(ft, ctx, 0))
                return false;
        }
    }
    return true;
}

// §3.6 / §35.7 (v0.276, #3333): the choice twin of
// recordHasUserDeclaredEquality. Returns true iff the choice carries a
// HOMOGENEOUS `=`/`<>` overload AND the §25.2 co-declared `Equals(T)` +
// `GetHashCode` pair — i.e. the user has discharged the §25.2 obligation
// for taking over equality. Per §3.6's hashable row, a choice with that
// surface is **hashable** (v0.276 widened the row to choices; pre-#3333 the
// choice arm OR'd nothing and a non-auto-hashable choice was unrescuable).
//
// Gate: applies ONLY to value-like choices. An owning or borrow-bearing
// choice with the co-declared pair is NOT hashable (§3.6: "owning and
// borrow-bearing choices are not hashable"; §35.7's rescue clause
// contemplates the operators on every classification but the §3.6 hashable
// row narrows the rescue to value-like — the only path through which
// equality is meaningful, since non-value-like choices carry no value
// equality contract).
//
// Imported choices (no local AST — classifyChoiceType returns nullopt) are
// conservatively treated as NOT having user-declared equality: matching
// isAutoEqualityBearingChoice's conservative-ACCEPT posture on the
// opposite side. Per-instantiation discipline (the §28.1 erasure of `owned`
// payload markers) re-checks concrete generic arguments at each
// instantiation, exactly as in isAutoEqualityBearingChoice.
//
// v0.276 fix #3333: this predicate did not exist before this revision.
// Before, the choice arm of isEqualityBearing was `return
// isAutoEqualityBearingChoice(...)` with no OR clause — a non-auto-
// hashable choice could not be hashable, and the spec's rescue clause was
// structurally unreachable. The implementation gate at #3333 flagged this
// defect class (#3144/#3152 family F9's cousin — empty-source predicate
// silently mis-answers).
bool choiceHasUserDeclaredEquality(const TypeSymbol* ch,
                                   SemaContext& ctx,
                                   const TypeRef* inst) {
    if (!ch || ch->kind != ast::TypeDeclKind::Choice) return false;
    // §3.6 hashable row: gate on ValueLike. Same type-ref setup as
    // isAutoEqualityBearingChoice so the §28.1 per-instantiation erasure
    // of `owned` payload markers applies — a `Result<int, string>`
    // instantiation classifies value-like (markers erase over int/string)
    // and reaches the body-member scan, while `Result<Connection, string>`
    // classifies owning and short-circuits to false.
    TypeRef bare;
    const TypeRef* rt = inst;
    if (!rt || rt->kind != TypeRef::Kind::Named || rt->declaration != ch) {
        bare.kind = TypeRef::Kind::Named;
        bare.declaration = const_cast<TypeSymbol*>(ch);
        rt = &bare;
    }
    auto verdict = common::classifyChoiceType(rt, ctx);
    if (!verdict) return false;  // imported — conservative REJECT (mirrors
                                  // isAutoEqualityBearingChoice's ACCEPT on
                                  // the opposite half — never rule in or out
                                  // a type we cannot inspect for the
                                  // opposing claim).
    if (*verdict != common::ChoiceClassification::ValueLike)
        return false;  // owning / borrow-bearing — not hashable per §3.6
    // Body-member scan — mirror of recordHasUserDeclaredEquality's
    // predicate, with the §25.2 pair added. The homogeneous-only `=`/`<>`
    // gate uses isHomogeneousEqualityOperatorOverload so a heterogeneous-
    // operand overload (`T = U`) does NOT trip the disqualifier (mirror
    // of fix #3326's record rule). Equals(T) and GetHashCode are the
    // enum spelling — §35.2 carries §10.5's plain-member replacement rule
    // over to choices, so `Equals`/`GetHashCode` carry no `override`
    // modifier.
    bool hasEquals = false, hasGetHashCode = false, hasHomoOp = false;
    for (const Symbol* m : ch->members) {
        if (auto* meth = dynamic_cast<const MethodSymbol*>(m)) {
            if (meth->isOperator &&
                isHomogeneousEqualityOperatorOverload(meth, ch, ctx))
                hasHomoOp = true;
            else if (!meth->isOperator && meth->name == "Equals" &&
                     meth->params.size() == 1)
                hasEquals = true;
            else if (!meth->isOperator && meth->name == "GetHashCode" &&
                     meth->params.empty())
                hasGetHashCode = true;
        }
    }
    return hasHomoOp && hasEquals && hasGetHashCode;
}

// §3.6 / §8.3 (v0.158, #1338): see Symbol.hpp. A type is hashable unless
// it is a record that neither synthesizes equality nor declares user equality,
// or a choice that is not value-like auto-hashable (§35.7, v0.214).
bool isEqualityBearing(const TypeRef* ty, SemaContext& ctx) {
    if (!ty) return true;                              // unresolved: conservative
    // The underlying type carries the equality contract; a nullable key `K?`
    // is hashable iff `K` is (the null-safe `=` of §8.4 handles null).
    if (ty->kind != TypeRef::Kind::Named || !ty->declaration)
        return true;  // primitive / string / unit / inline array / function /
                      // unresolved (generic param, imported builtin) — all bear
                      // consistent equality.
    const TypeSymbol* ts = ty->declaration;
    if (ts->kind == ast::TypeDeclKind::Record) {
        // A record: hashable iff it synthesizes equality (positional /
        // combined §9.1/§9.3, or an auto-hashable body record §9.2) or
        // declares user equality (an `operator =`/`<>` overload — its §25.2
        // co-declared `Equals(T)`/`GetHashCode` companions are separately
        // enforced).
        // §9.3 (v0.158, #1383): positional/combined records must pass the
        // admissibility check; the blind `return true` was the pre-#1383 bug.
        if (recordIsPositional(ts))
            return isPositionalRecordEqualityBearing(ts, ctx) ||
                   recordHasUserDeclaredEquality(ts);
        return isAutoEqualityBearingBodyRecord(ts, ctx) ||
               recordHasUserDeclaredEquality(ts);
    }
    // §35.7 (v0.210/v0.214, #2083/#2087): a CHOICE is hashable exactly
    // when it is auto-hashable — i.e. value-like with all case payloads
    // admissibly typed (the RECURSIVE §35.7 rule: "this same list applied to
    // every case's payload fields"; classification alone under-enforces — a
    // value-like choice with a function/Object payload is value-like but not
    // hashable, the key-position half of #2110). A borrow-bearing or
    // owning choice carries no value equality contract and is NOT
    // hashable (Dictionary<Outcome, _> keys fail here per §3.6). This
    // one computation also decides the §14.2 `where T: hashable` constraint
    // (#2087) — "the constraint adds no second definition of the property" —
    // and the §3.6 rule-2 hash-container-key check, so the constraint, the
    // key check, and the use-site `=`/`<>` error can never disagree. A
    // primitive, string, class, interface, function, enum, or inline array is
    // hashable (the `return true` fall-through applies).
    if (ts->kind == ast::TypeDeclKind::Choice)
        // §3.6 / §35.7 (v0.276, #3333): a choice is hashable iff it is
        // auto-hashable OR carries the §25.2 co-declared equality surface
        // (a value-like choice with homogeneous `=`/`<>` overload AND
        // `Equals(T)`/`GetHashCode`). Pre-#3333 the choice arm OR'd
        // nothing — a non-auto-hashable choice was unrescuable, the exact
        // defect the v0.276 spec ruling records against the pre-ruling
        // implementation. The two predicates are mutually exclusive on
        // legal input (a choice with `=`/`<>` + Equals/GetHashCode fails
        // isAutoEqualityBearingChoice's overload scan), so the OR is
        // exact — no risk of a double-counted verdict.
        // #2047: pass `ty` (the instantiation, with genericArgs) so a generic
        // choice is judged at its instantiation (`Result<int, string>` is
        // auto-hashable; its `owned` markers erase per §28.1).
        return isAutoEqualityBearingChoice(ts, ctx, ty) ||
               choiceHasUserDeclaredEquality(ts, ctx, ty);
    return true;  // class (reference identity), interface, enum — all bear
                  // consistent Equals/GetHashCode.
}

// §14.2 (v0.214, #2087): the component-path fragment for a FAILED
// `where T: hashable` constraint — see Symbol.hpp. Precondition:
// isEqualityBearing(ty, ctx) == false. The hashable property surfaces
// its failure at the top-level type (a non-auto-hashable record, or a
// non-value-like choice), so the "component path" is the offending type + the
// §3.6/§9.2/§35.7 reason — the analog of sendableFailureComponentPath, at the
// detail level §3.6 supports for this property. Returns empty (the caller keeps
// the flat message) when the type is not the record/choice failure shape.
std::string hashableFailureComponentPath(const TypeRef* ty,
                                         SemaContext& ctx) {
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
               " (§35.7), not value-like auto-hashable";
    }
    if (ts->kind == ast::TypeDeclKind::Record) {
        return prefix + "record `" + std::string(ts->name) +
               "` is neither auto-hashable (§9.2) nor declares custom "
               "equality (`operator =`/`<>` with `Equals(T)`/`GetHashCode`, "
               "§25.2)";
    }
    return {};  // class/interface/enum/primitive are all hashable —
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
