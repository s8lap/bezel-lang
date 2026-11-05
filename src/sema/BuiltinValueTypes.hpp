#pragma once

// The BUILT-IN stdlib VALUE-RECORD type predicates — the ones sema and
// codegen must AGREE on, because one of them ADMITS a program and the other
// LOWERS it.
//
// WHY THIS HEADER EXISTS. stdlib §2.6 (v0.355) declares
//
//     public record Guid: IComparable<Guid>, IEquatable<Guid>
//
// with "CompareTo = lexicographic over the 16 canonical big-endian bytes".
// So `Guid` satisfies the `where T: IComparable<T>` constraint that stdlib
// §4.4 puts on `Array<T>.Sort()` / `Sorted()` and that spec §14.2 admits
// ("records ... implementing it").
//
// A BUILT-IN value record carries no user `TypeDecl`, so the class/interface
// arms of sema's constraint gate — which read `baseTypeRefs` — never see the
// conformance, and the gate REFUSED `Array<Guid>.Sort()` with "Guid does not
// implement IComparable<Guid>" (issue #8237). The same shape was fixed for
// stdlib §2.5's temporal records by #7766.
//
// THE TWO SIDES MUST AGREE, or the fix buys a WORSE defect than the refusal
// it removes. Sema's gate ADMITS the element; codegen then has to ORDER it
// by §2.6's rule. A codegen that failed to recognise an admitted `Guid`
// would fall through to the `bz$intcmp` fallback — an i32 LOAD out of a
// 16-byte value — and sort the array by the low half of the Guid's first
// word. A silent WRONG ORDER is worse than a refusal: the program compiles,
// runs, and answers something false.
//
// ⇒ The predicate below is THE ONE definition, consulted by BOTH sides.
//
// A USER type named `Guid` — locally declared, or imported from another
// module — is NOT the built-in: `isUserDeclaredTypeSym` (sema/Symbol.hpp) is
// the house discriminator for that (#5272's rule: a real `ast::TypeDecl`, or
// a populated `moduleName`).
//
// A NULLABLE element never satisfies a type bound: §14.2 — "a type-bound
// constraint — naming a class or interface — implies non-null T".

#include <string_view>

#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// stdlib §2.6 (v0.355): is `t` the BUILT-IN `Guid` value record?
//
// The three shapes a built-in reaches a program in:
//
//   (a) the synthetic `builtinClassSym("Guid")` — a TypeSymbol with no
//       `decl` and no `moduleName` (SemaContext::builtinClassSym);
//   (b) the DECLARATION-LESS Named ref the built-in-name admission in
//       TypeResolution produces — the type of a `Guid`-annotated local,
//       parameter or field (see #1463's note: the value builtins keep
//       `declaration = null` so `lowerNew`'s closed-constructor gate fires);
//   (c) the metadata spelling (`importedName`) of a type reconstructed from
//       another module's metadata.
//
// (a) and (b) are the two a program actually meets for `Guid`.
inline bool isStdlibGuidType(const TypeRef* t) {
    if (!t || t->kind != TypeRef::Kind::Named) return false;
    if (t->nullable) return false;  // §14.2: a type bound implies non-null T
    if (t->declaration) {
        if (isUserDeclaredTypeSym(t->declaration)) return false;  // a user type
        return t->declaration->name == "Guid";
    }
    // The DECLARATION-LESS shape: the name is the LAST part of the
    // (possibly qualified) spelling — `System.Guid` names the same built-in
    // as `Guid` (§2.6's module is `System`).
    if (t->astNode && t->astNode->name && !t->astNode->name->parts.empty())
        return t->astNode->name->parts.back() == "Guid";
    if (!t->astNode && !t->importedName.empty())
        return t->importedName == "Guid";
    return false;
}

}  // namespace bezel::sema
