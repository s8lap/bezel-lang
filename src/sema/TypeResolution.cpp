#include "sema/TypeResolution.hpp"
#include "sema/TypeResolution_impl.hpp"

#include <unordered_map>
#include <unordered_set>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// The Resolver worker class is declared in TypeResolution_impl.hpp; its
// method definitions live in the sibling TypeResolution_*.cpp translation
// units (TypeResolution_Names.cpp for type/name resolution,
// TypeResolution_Checks.cpp for the well-formedness checks). This file holds
// the public entry function plus the resolver's top-level driver.

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
    // §3.4, #884: PascalCase System type-name aliases.  Each maps to the same
    // PrimitiveType as its lowercase keyword counterpart, so `String`, `Int32`,
    // `Double`, etc. resolve identically to `string`, `int`, `double`, etc. in
    // type positions, explicit-cast positions, and static-member dispatch.
    if (name == "Boolean") return PrimitiveType::Bool;
    if (name == "Char")    return PrimitiveType::Char;
    if (name == "String")  return PrimitiveType::String;
    if (name == "Byte")    return PrimitiveType::Byte;
    if (name == "SByte")   return PrimitiveType::SByte;
    if (name == "Int16")   return PrimitiveType::Short;
    if (name == "UInt16")  return PrimitiveType::UShort;
    if (name == "Int32")   return PrimitiveType::Int;
    if (name == "UInt32")  return PrimitiveType::UInt;
    if (name == "Int64")   return PrimitiveType::Long;
    if (name == "UInt64")  return PrimitiveType::ULong;
    if (name == "Single")  return PrimitiveType::Float;
    if (name == "Float")   return PrimitiveType::Float;   // §3.4 / stdlib §9.2 (#1241)
    if (name == "Double")  return PrimitiveType::Double;
    if (name == "Decimal") return PrimitiveType::Decimal;
    return PrimitiveType::None;
}

// §7.9 / §22.4 (v0.181): see the declaration in TypeRef.hpp.
bool isNonNullableReferenceType(const TypeRef* t) {
    if (!t || t->nullable) return false;
    switch (t->kind) {
        case TypeRef::Kind::Primitive:
            return t->primitive == PrimitiveType::String ||
                   t->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;
        case TypeRef::Kind::Named:
            // A resolved class/interface has no default; a resolved record/enum
            // is a value type. A Named TypeRef with no declaration is an
            // imported/builtin reference type (a collection wrapper, `Task`, …)
            // or a bare generic parameter, both of which follow the
            // non-nullable-reference rule (§7.10).
            if (!t->declaration) return true;
            return t->declaration->kind == ast::TypeDeclKind::Class ||
                   t->declaration->kind == ast::TypeDeclKind::Interface;
        default:  // Unit, InlineArray, Error
            return false;
    }
}

void Resolver::run(const ast::SourceFile& src) {
    // §2.8.1 rule 4 (v0.188): "An `import` naming the enclosing file's own
    // module is rejected ('a module cannot import itself')." The enclosing
    // file's own module name is whatever this file's own `module Name;`
    // declares — computed once here so every `import` below can be compared
    // against it. A self-loop is not the legal cross-module SCC cycle rule 4
    // carves out; it is rejected unconditionally.
    std::string selfModuleName;
    if (src.module && src.module->name) {
        for (std::size_t i = 0; i < src.module->name->parts.size(); ++i) {
            if (i != 0) selfModuleName += '.';
            selfModuleName += std::string(src.module->name->parts[i]);
        }
    }

    // §2.15 / issue #631: build the set of imported namespace names so that
    // import-gated builtin types (BinarySerializer / BinaryDeserializer) can be
    // checked in resolveNamed. Every `import X;` contributes its dotted name.
    // §2.2 v0.164 / #1365: also collect import aliases for qualified-name
    // resolution in resolveNamed (§2.7).
    sourceImports_.clear();
    importAliases_.clear();
    ambiguousImportNames_.clear();
    for (const auto& imp : src.imports) {
        if (!imp || !imp->name) continue;
        std::string dotted;
        for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
            if (i != 0) dotted += '.';
            dotted += std::string(imp->name->parts[i]);
        }
        // §2.8.1 rule 4: reject an `import` naming this very file's own
        // module. Diagnose and skip — do not add the self-import to
        // sourceImports_/importAliases_, so a self-import behaves as absent
        // (not as a tautological self-visible import) for the rest of name
        // resolution, matching the "rejected" verdict rather than inventing
        // a fallback semantics (proposal 0034 §1: the compiler's prior
        // undefined behavior of silently exposing the same-named artifact's
        // publics is exactly the third, nowhere-specified meaning rule 4
        // forecloses).
        if (!selfModuleName.empty() && dotted == selfModuleName) {
            ctx_.diagnostics().report(
                Severity::Error, imp->span.begin,
                std::string("module `") + selfModuleName +
                "` cannot import itself (\xC2\xA7""2.8.1 rule 4)");
            continue;
        }
        sourceImports_.insert(dotted);
        // §2.2 v0.164: record the alias → module name mapping.
        if (!imp->alias.empty()) {
            importAliases_[imp->alias] = dotted;
        }
    }
    Scope moduleScope;
    // Populate module-scope: every top-level type / alias by name.
    // #2131 / §2.3 v0.226: a local top-level declaration of the same name as
    // an auto-imported prelude type SHADOWS the prelude (the prelude is
    // prepended into `unit->topDecls` by Driver::injectStdlibPrelude, but the
    // user's own decl sits AFTER it in iteration order; inserting it here
    // OVERWRITES the prelude's slot so resolveNamed sees only the user decl).
    // §2.2 reinforces the same rule for explicit `import` — locals win, imports
    // never shadow. A reader of the loop below who skips this comment would
    // read "child->name may collide with a prelude decl" as a bug; the pin
    // is the §2.3 paragraph "the other always-available names... may be
    // shadowed by local declarations under the normal name-resolution rules".
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children) {
            if (!child->name.empty()) {
                moduleScope.names[child->name] = child;
            }
        }
    }
    // §2.2 imports (issue #328 inc 3): make the public names of each imported
    // module visible in this file. For every `import X;` whose module X was
    // loaded into the context (a dependency artifact the driver passed), expose
    // X's exported top-level types by simple name in module scope, so a
    // reference to one resolves to the imported TypeSymbol. A local declaration
    // of the same name takes precedence (it was inserted above); imports never
    // shadow a local type (§2.2: imports make names *visible*, locals win).
    // §2.2 v0.164 collision rule (#1365): when two imported modules export the
    // same public name, that bare name is ambiguous — using it unqualified is a
    // compile error. Track each name's origin module; on collision, remove it
    // from scope and record it in ambiguousImportNames_ so resolveNamed can
    // diagnose a clear "ambiguous" error rather than "unresolved identifier".
    // Only engaged when imports were actually loaded — the no-import path is
    // byte-for-byte unchanged.
    if (ctx_.hasImports()) {
        // Snapshot local module declarations already in scope: these shadow
        // imports and are never marked ambiguous (§2.2: local wins silently).
        std::unordered_set<std::string_view> localNames;
        for (const auto& [name, _] : moduleScope.names)
            localNames.insert(name);
        // Track which module first introduced each bare name via import.
        std::unordered_map<std::string, std::string> importedNameOrigin;
        for (const auto& imp : src.imports) {
            if (!imp || !imp->name) continue;
            std::string dotted;
            for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
                if (i != 0) dotted += '.';
                dotted += std::string(imp->name->parts[i]);
            }
            // §2.8.1 rule 4: a self-import was already diagnosed above (and
            // excluded from sourceImports_). Also skip it here so a same-
            // named DEPENDENCY ARTIFACT — the issue #1730 second repro shape
            // (`module Dup; import Dup;` built against a `dup1.o` that also
            // declares `module Dup;`) — never gets its publics exposed into
            // this file's scope. Without this, the self-import diagnostic
            // above still fires, but the artifact's types remained visible
            // anyway: a third, nowhere-specified semantics (neither the
            // spec's literal no-op reading nor rejection) that proposal 0034
            // §1 calls out as the bug to close.
            if (!selfModuleName.empty() && dotted == selfModuleName) continue;
            const std::vector<TypeSymbol*>* exported =
                ctx_.importedTypesOf(dotted);
            if (!exported) continue;
            for (TypeSymbol* t : *exported) {
                if (t->name.empty()) continue;
                // §2.2: local declarations shadow imports — do not overwrite.
                if (localNames.count(std::string_view(t->name))) continue;
                auto [originIt, inserted] =
                    importedNameOrigin.emplace(std::string(t->name), dotted);
                if (inserted) {
                    // First import to export this name — add it to scope.
                    moduleScope.names.emplace(std::string_view(t->name), t);
                } else if (originIt->second != dotted) {
                    // §2.2 collision: a second (different) import exports this
                    // name. Remove it from scope and record as ambiguous.
                    ambiguousImportNames_.insert(std::string(t->name));
                    moduleScope.names.erase(std::string_view(t->name));
                }
                // else: same module listed twice (no-op).
            }
        }
        // §2.2 (v0.183, #1515 / #1489): an import alias may not collide with an
        // unambiguously imported public bare name. §2.7 rule 1 resolves
        // imported bare names before import aliases with no backtracking, so a
        // same-named alias would be unreachable in every chain-head position —
        // the collision is an error at the alias declaration rather than a
        // silently dead alias. A name is unambiguously imported iff some import
        // exported it (importedNameOrigin) and no second import made it
        // ambiguous (ambiguousImportNames_). A name shadowed by a local
        // declaration was never added to importedNameOrigin, so the alias/local
        // collision — a distinct entry in §2.2's list — is not diagnosed here.
        // §2.2 (v0.226, #2179): aliases are declarations, so each alias may
        // be bound only once in a file. Keep the first module for each alias so
        // a later declaration can name both conflicting modules.
        std::unordered_map<std::string, std::string> aliasOrigin;
        for (const auto& imp : src.imports) {
            if (!imp || !imp->name || imp->alias.empty()) continue;
            std::string dotted;
            for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
                if (i != 0) dotted += '.';
                dotted += std::string(imp->name->parts[i]);
            }
            auto [aliasIt, inserted] = aliasOrigin.emplace(imp->alias, dotted);
            if (!inserted) {
                ctx_.diagnostics().report(
                    Severity::Error, imp->span.begin,
                    std::string("import alias `") + imp->alias +
                    "` for module `" + dotted +
                    "` collides with the same alias already bound to module `" +
                    aliasIt->second + "`; choose a different alias (\xC2\xA7""2.2)");
                continue;
            }
            // §2.2 (v0.226, #2213): an import alias also occupies the
            // current module's declaration namespace. A local type wins over
            // an imported bare name for lookup, but the alias is still a
            // declaration-site collision rather than a silently shadowed
            // qualifier. `localNames` is the snapshot taken before imports,
            // so it covers all declarations already present in this module
            // scope when name resolution starts.
            if (localNames.count(imp->alias)) {
                ctx_.diagnostics().report(
                    Severity::Error, imp->span.begin,
                    std::string("import alias `") + imp->alias +
                    "` for module `" + dotted +
                    "` collides with type `" + imp->alias +
                    "` declared in the current module; choose a different "
                    "alias (\xC2\xA7""2.2)");
                continue;
            }
            auto originIt = importedNameOrigin.find(imp->alias);
            if (originIt == importedNameOrigin.end()) continue;
            if (ambiguousImportNames_.count(imp->alias)) continue;
            ctx_.diagnostics().report(
                Severity::Error, imp->span.begin,
                std::string("import alias `") + imp->alias +
                "` collides with the unambiguously imported public bare name `" +
                imp->alias + "` (from module `" + originIt->second +
                "`); \xC2\xA7""2.7 rule 1 resolves imported bare names before "
                "aliases with no backtracking, so the alias would be "
                "unreachable — choose a different alias (\xC2\xA7""2.2)");
        }
    }
    for (const auto& decl : src.topDecls) {
        if (!decl) continue;
        resolveTopDecl(*decl, &moduleScope);
    }
    // §14.2: now that every instantiation site and every deferred
    // nested-generic obligation has been recorded, join them and re-check the
    // inner constraints with the outer parameter bound to each concrete
    // argument. This closes the soundness hole where a constraint on an inner
    // generic (`C<T> where T: new()`) was silently skipped when reached
    // through an outer generic (`Box<W>` ⇒ `C<W>`, then `Box<IFoo>`).
    checkDeferredNestedConstraints();
    // §11.8 / §14.2 (v0.176, #807): re-check type-bound failures deferred
    // during base-list resolution (the recurrence `class Meters:
    // INumber<Meters>` cannot see its own conformance until baseTypes are
    // fully populated). Runs after checkDeferredNestedConstraints so entries
    // that pass appends there are flushed too.
    recheckDeferredTypeBounds();
    // Slice S12: after every base-type reference is resolved,
    // walk each TypeSymbol's inheritance chain and report cycles.
    bool hadCycle = checkInheritanceCycles();
    // Slice S13: a concrete class must implement every inherited
    // abstract / interface member. Skipped when a cycle exists
    // (the chain walk wouldn't terminate cleanly).
    if (!hadCycle) checkAbstractImplementations();
    // §11.8 (v0.176, #807): a class/record conforming to an interface with
    // STATIC members must supply matching statics (exact signature).
    if (!hadCycle) checkStaticInterfaceConformance();
    // Slice S15: hiding forbidden / override must target a virtual.
    if (!hadCycle) checkHidingAndOverride();
    // Slice S14: duplicate non-overloadable member names within a
    // single type.
    checkDuplicateMembers();
    // §11.3 / §13.2 (#747 R42): two top-level types may not share a name.
    checkDuplicateTopLevelTypes();
    // Slice S16: a static class may contain only static members.
    checkStaticClassMembers();
    // §23.5 (#747): static indexers are not supported (any type).
    checkStaticIndexers();
    // §23.5 (v0.241, #2758): indexer parameters may not declare defaults.
    checkIndexerParamDefaults();
    // §25.2 (#746): comparison/equality operators must form a complete group.
    checkOperatorGroups();
    // §8.3 (#1193): a type overriding `Equals` must also override `GetHashCode`.
    checkEqualsHashCodePairing();
    // §25.2 (v0.158, #1338): a record/enum overloading `=`/`<>` (homogeneous
    // operands) must also declare `Equals(T)` + `override GetHashCode`.
    checkEqualityCoDeclaration();
    // §3.6 / §8.3 (v0.158, #1338): hash-container keys/elements must be
    // hashable (runs after all member types are resolved).
    checkHashContainerKeys();
    // stdlib §4.2 / §14.4 / §3.6 rule 2 (#2713): an owning collection whose
    // element slot is a bare generic parameter is re-checked against the type
    // each instantiation of the enclosing generic binds (runs after all member
    // types are resolved, so every instantiation has been recorded).
    checkOwningElementInstantiations();
    // §28.11 / §15.3 (#3775): every recorded element-category slot must not
    // be a move-only value type (an owning inline array or an owning-
    // classified choice) — runs after checkOwningElementInstantiations for
    // the same reason: every instantiation site has been recorded by now,
    // and a choice's classification needs every member type resolved.
    checkElementCategoryInstantiations();
    // §14.4 / §15.3 (#3794): OwningBuffer<T>/Span<T>/ReadOnlySpan<T> reject
    // the §15.3 move-only kinds (owning inline arrays, owning-classified
    // choices) per the §14.4 exception's own clause. Pass 1 (concrete slot)
    // runs inline at the annotation site in TypeResolution_Names.cpp; Pass 2
    // here re-walks bare generic parameters of the enclosing type against
    // each instantiation — same dependencies as checkElementCategory-
    // Instantiations above (every instantiation site must be recorded by
    // now, and a choice's classification needs every member type resolved).
    checkOwningBufferSpanMoveOnlyInstantiations();
    // §17.4 / §3.6 rule 2 (#2911, #2962): `x as T` over a TYPE-level parameter
    // is re-checked against the type each instantiation of the enclosing
    // generic binds (runs here for the same reason: every instantiation site
    // has been recorded by now).
    checkAsTargetInstantiations();
    // Slice S17: class-level modifier mutual-exclusions.
    checkClassModifierCombinations();
    // fix #3100 (§19.6 / §11.7, v0.254): a `closed` class whose hierarchy
    // has a non-public concrete subclass makes the cross-module
    // exhaustiveness guarantee unsound (the leaf is invisible to importers
    // but can still be a runtime value of the closed type). Runs after
    // checkClassModifierCombinations because the descriptor needs the
    // whole module's class set collected (the descendants of a `closed`
    // root are not necessarily textually adjacent).
    checkClosedHierarchyLeafVisibility();
    // Slice S19: overloads must differ in signature.
    checkOverloadSignatures();
    // Slice S22: top-level types may only be public or internal.
    checkTopLevelVisibility();
    // Slice S23: record restrictions (§9.7).
    checkRecordRestrictions();
    // #2208 (§35.1, v0.226): choice self-containment through value-typed
    // payload components (record↔choice by-value cycles, mutable mutual pairs
    // of choices, etc.). Walks the same value-containment graph as the
    // §9 record check; the cycle's choice-side diagnostic is reported here.
    checkChoiceSelfContainment();
    // §9.8 (issue #550): ISerializable/IDeserializable conformance eligibility.
    checkSerializationEligibility();
    // #594 (§28.2): user code may not declare Dispose() on a class/record.
    checkUserDeclaredDispose();
    // fix #3032 (§15.4.1, v0.262): public / owned C<T> return / both-forms
    // declaration-site diagnostics for a static `FromCollectionLiteral`
    // factory. Runs after every parameter / return type has been resolved
    // so the carrier identification and return-type shape checks can read
    // `ctx_.resolvedType(...)` (the same prerequisite every other post-
    // type-resolution check has).
    checkCollectionLiteralFactoryConjuncts();
    // Slice S28: accessor visibility (§13.6).
    checkAccessorVisibility();
    // Slice S30: const member type must be primitive or string.
    checkConstTypes();
    // Slice S32: enum member restrictions (§10.5).
    checkEnumRestrictions();
    // §14.2 (#746): contradictory generic constraints (e.g. class + value).
    checkGenericConstraintConsistency();
    // Slice S33: method body-presence rules (§12.5 / §11.2).
    checkMethodBodies();
    // Slice S34: static members may not be virtual (§12.1).
    checkStaticNotVirtual();
    // Slice S35: `async abstract` is a compile error (§12.6).
    checkAsyncModifiers();
    // §22.15 (v0.202, #2052): `pure` placement + signature restrictions.
    checkPureModifiers();
    // §22.15 (v0.202, #2052): `pure` dispatch matching (override + iface
    // implementation). Rule-1 checks run inline in checkHidingAndOverride
    // and checkAbstractImplementations (which ran earlier above); this
    // walker is a placeholder for future chain-wide audits.
    checkPureDispatch();
    // §23.2 (v0.214, #2088, proposal 0058): `readonly` is meaningful only on
    // auto-properties and interface properties — anywhere body-bearing it is
    // either redundant (shape expresses read-onlyness) or contradictory
    // (block form with a `set` accessor). The grammar admits `readonly?` on
    // the body-bearing productions as an over-admission; this pass performs
    // the §23.2 semantic rejection.
    checkReadonlyPropertyForms();
    // Slice S36: constructor chain-call placement (§12.8).
    checkConstructorChains();
    // Slice S37: static constructor takes no parameters (§22.7).
    checkStaticConstructorParams();
    // Slice S38: base/interface visibility (§13.7).
    checkBaseVisibility();
    // §13.3 (#956): a member's signature types must be at least as visible as
    // the member (private-type-in-non-private-member case).
    checkMemberSignatureVisibility();
    // §12.9 / issue #431: variance position enforcement on interface members.
    // `out T` must appear only in output positions; `in T` only in input.
    checkVariancePositions();
    // §12.9 / §11.8 (v0.181, issue #1559): variance markers (`in`/`out`) are
    // interface-only syntax — reject them on class/record/enum/method/alias
    // parameters, and on an interface's parameters when it declares a
    // static member.
    checkVarianceMarkerScope();
    // §22.12 rules 2 and 4 (v0.163, #1363): statics-and-domains checks.
    // Rule 2 errors mutable static class/interface fields; rule 4 warns
    // static readonly class/interface fields. Runs after type resolution
    // so field types are available.
    checkStaticFieldDomains();
    // §22.12 rule 3/4 (v0.262, #3272): pass 2 for the `static readonly`
    // bare-parameter classification. Walks the recorded instantiations of
    // each enclosing type whose body declared a `static readonly` field/
    // auto-property at a bare parameter (`T`, `T?`), and applies rule 3
    // (immutable content → clean), rule 4 (class-typed warning, suppressed
    // for the warning-suppression set), or the residual paragraph's per-
    // instantiation cases (interface, function, borrow-bearing record,
    // holds-borrows choice, owning choice → rejection, owning-inline-array
    // → default warning). The diagnostic lands at the instantiation site
    // per §3.6 rule 2. Runs here for the same reason as
    // checkOwningElementInstantiations / checkAsTargetInstantiations above:
    // every instantiation site has been recorded by now.
    checkStaticReadonlyBareTInstantiations();
    // §14.6 (v0.262, #3271): expanding in-SCC instantiation edges are a
    // compile error at the declaration. Runs after every instantiation
    // has been recorded (instantiations_ is fully populated by now) so
    // the SCC walk sees the whole graph. Placed LAST so the diagnostic
    // lands after every other declaration-site check has fired.
    checkExpandingInstantiationEdges();
    // §14.6 (v0.262, #3271), generic-MEMBER half: the same per-edge condition
    // on a generic member's SELF-edge (`M<T>`'s body spelling
    // `M<Pair<T, T>>(…)`), which the type-node graph above cannot see.
    // Immediately after its sibling so the two §14.6 verdicts are adjacent in
    // both the source and the diagnostic order.
    checkExpandingGenericMemberEdges();
}

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx) {
    Resolver r(ctx);
    r.run(src);
    return !ctx.diagnostics().hasErrors();
}

// §9.8 / #2325: see the declaration comment (TypeResolution.hpp) and
// Resolver::checkClassSerializationEligibilityLate's own comment
// (TypeResolution_impl.hpp) for why this runs as a separate late pass.
bool checkClassSerializationEligibilityLate(const ast::SourceFile& src,
                                            SemaContext& ctx) {
    (void)src;
    Resolver r(ctx);
    r.checkClassSerializationEligibilityLate();
    return !ctx.diagnostics().hasErrors();
}

// §30.14 / §14.2 / #3235: see the declaration comment (TypeResolution.hpp)
// and Resolver::checkSendableConstraintsLate's own comment
// (TypeResolution_Checks.cpp) for why this runs as a separate late pass.
// Same pattern as `checkClassSerializationEligibilityLate`: a fresh
// Resolver wrapping the same `ctx` replays the deferred sites; the
// pending-sendable list is local to each Resolver instance, so this is
// safe to call from any `typecheckExpressions()` pipeline.
bool checkSendableConstraintsLate(const ast::SourceFile& src,
                                  SemaContext& ctx) {
    (void)src;
    Resolver r(ctx);
    r.checkSendableConstraintsLate();
    return !ctx.diagnostics().hasErrors();
}

// §14.2 / fix #3160: §14.2 constraint check at an instantiation site
// without an `ast::Type` node — the receiver of a static-member access
// (`Ferry<Bad>.Count()`). Same pattern as `checkSendableConstraintsLate`:
// a fresh Resolver wrapping `ctx` runs the check; deferred state is
// local to this Resolver and goes unused (already flushed by the
// resolveTypes-time Resolver before ExprTC calls this), but the
// common-shape case (concrete typeArgs) is handled. See the comment on
// `Resolver::checkGenericConstraintsAtUseSite` for the full rationale
// and the deferred-nested edge case.
bool checkGenericConstraintsAtExpr(SemaContext& ctx,
                                   SourceLocation useSite,
                                   const std::vector<const ast::Type*>& argAsts,
                                   const std::vector<TypeRef*>& argRefs,
                                   TypeSymbol* tsym) {
    Resolver r(ctx);
    r.checkGenericConstraintsAtUseSite(useSite, argAsts, argRefs, tsym);
    return !ctx.diagnostics().hasErrors();
}

// §22.12 rule 4 residual + §9.7 static-field bullet (v0.265, #3273 /
// #3290): the §9.7 null-source value-site pass. Same pattern as
// `checkSendableConstraintsLate` and `checkClassSerializationEligibilityLate`:
// a fresh Resolver wrapping `ctx` walks every static readonly field /
// auto-property whose declared type is one of §9.7's three deferred
// kinds and verifies the initializer is the null-source form. Called
// from `typecheckExpressions()` (after the expression walk has named
// every positional argument) so the constructor-shape analysis has
// something to walk.
bool checkStaticReadonlyNullSource(SemaContext& ctx) {
    Resolver r(ctx);
    r.checkStaticReadonlyNullSource();
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
