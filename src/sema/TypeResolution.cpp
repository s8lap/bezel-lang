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

void Resolver::run(const ast::SourceFile& src) {
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
        sourceImports_.insert(dotted);
        // §2.2 v0.164: record the alias → module name mapping.
        if (!imp->alias.empty()) {
            importAliases_[imp->alias] = dotted;
        }
    }
    Scope moduleScope;
    // Populate module-scope: every top-level type / alias by name.
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
    // §25.2 (#746): comparison/equality operators must form a complete group.
    checkOperatorGroups();
    // §8.3 (#1193): a type overriding `Equals` must also override `GetHashCode`.
    checkEqualsHashCodePairing();
    // §25.2 (v0.158, #1338): a record/enum overloading `=`/`<>` (homogeneous
    // operands) must also declare `Equals(T)` + `override GetHashCode`.
    checkEqualityCoDeclaration();
    // §3.6 / §8.3 (v0.158, #1338): hash-container keys/elements must be
    // equality-bearing (runs after all member types are resolved).
    checkHashContainerKeys();
    // Slice S17: class-level modifier mutual-exclusions.
    checkClassModifierCombinations();
    // Slice S19: overloads must differ in signature.
    checkOverloadSignatures();
    // Slice S22: top-level types may only be public or internal.
    checkTopLevelVisibility();
    // Slice S23: record restrictions (§9.7).
    checkRecordRestrictions();
    // §9.8 (issue #550): ISerializable/IDeserializable conformance eligibility.
    checkSerializationEligibility();
    // #594 (§28.2): user code may not declare Dispose() on a class/record.
    checkUserDeclaredDispose();
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
}

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx) {
    Resolver r(ctx);
    r.run(src);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
