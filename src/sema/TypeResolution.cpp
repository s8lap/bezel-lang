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
    sourceImports_.clear();
    for (const auto& imp : src.imports) {
        if (!imp || !imp->name) continue;
        std::string dotted;
        for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
            if (i != 0) dotted += '.';
            dotted += std::string(imp->name->parts[i]);
        }
        sourceImports_.insert(std::move(dotted));
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
    // Only engaged when imports were actually loaded — the no-import path is
    // byte-for-byte unchanged.
    if (ctx_.hasImports()) {
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
                // Local declarations win (do not overwrite an existing entry).
                moduleScope.names.emplace(t->name, t);
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
    // Slice S12: after every base-type reference is resolved,
    // walk each TypeSymbol's inheritance chain and report cycles.
    bool hadCycle = checkInheritanceCycles();
    // Slice S13: a concrete class must implement every inherited
    // abstract / interface member. Skipped when a cycle exists
    // (the chain walk wouldn't terminate cleanly).
    if (!hadCycle) checkAbstractImplementations();
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
}

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx) {
    Resolver r(ctx);
    r.run(src);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
