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
    return PrimitiveType::None;
}

void Resolver::run(const ast::SourceFile& src) {
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
    // Slice S16: a static class may contain only static members.
    checkStaticClassMembers();
    // Slice S17: class-level modifier mutual-exclusions.
    checkClassModifierCombinations();
    // Slice S19: overloads must differ in signature.
    checkOverloadSignatures();
    // Slice S22: top-level types may only be public or internal.
    checkTopLevelVisibility();
    // Slice S23: record restrictions (§9.7).
    checkRecordRestrictions();
    // Slice S28: accessor visibility (§13.6).
    checkAccessorVisibility();
    // Slice S30: const member type must be primitive or string.
    checkConstTypes();
    // Slice S32: enum member restrictions (§10.5).
    checkEnumRestrictions();
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
}

bool resolveTypes(const ast::SourceFile& src, SemaContext& ctx) {
    Resolver r(ctx);
    r.run(src);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
