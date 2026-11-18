#pragma once

// §26.5 rule 2 / §14.5 phase 1 — the BUILT-IN collection-to-interface
// relation, and the simple-name read it needs.
//
// fix #6828 (§14.5 / §2.8.2, v0.359): this table used to be a TU-internal
// `static` pair in sema/ExprTypecheck.cpp, so only sema could read it. The
// §14.5 phase-1 rule — "a constructed parameter type `G<P1, …, Pn>` unifies
// component-wise and invariantly against an argument of the same generic type,
// or against the unique instantiation of `G` that the argument's type
// implements or extends" — is a rule the INFERENCE (sema) and the
// cross-module generic gate (codegen) must apply alike: the gate decides
// whether the imported generic's type parameter is bound to a concrete
// non-`int` type, and it can only answer that from the SAME unification the
// inference used. The house rule is ONE implementation of a rule, so the
// table (and the simple-name read it needs) moves HERE, and both sides
// include this header.

#include <memory>
#include <string_view>
#include <vector>

#include "sema/Symbol.hpp"  // isUserDeclaredTypeSym (§26.5 rule 2)
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// §26.5 (#714): extract the simple name from a Named TypeRef — works for
// both user-declared types (via declaration->name) and null-declaration
// builtin types (via astNode->name->parts or importedName).
inline std::string_view namedTypeSimpleName(const TypeRef* ref) {
    if (!ref || ref->kind != TypeRef::Kind::Named) return {};
    if (ref->declaration) return ref->declaration->name;
    if (!ref->importedName.empty()) return ref->importedName;
    // fix #4570 (§27.8, v0.306): a type alias resolves TRANSPARENTLY by
    // copying the underlying TypeRef and then RESTORING astNode to the USE
    // SITE (the alias name `Al`), so the astNode read below answers `Al`
    // rather than the underlying builtin head. TypeRef.hpp names
    // `aliasResolvedHead` as the compensating field carried for exactly this
    // reason and `headName()` as "the ONE read every builtin-collection/
    // wrapper identity check should use instead of astNode->name->parts[0]";
    // this function is such a check and was missing the leg.
    //
    // Measured consequence: `type Al := Array<int>; var x: Al := src;`
    // reached `variantGenericAssignable` with toName="Al" against
    // fromName="Array", matched no ancestor of `Array<int>` under that name,
    // and — because `Array` IS in `builtinBorrowingCollectionFullyModeled` —
    // took that predicate's proof-of-non-conformance REJECT. That
    // contradicts §27.8's "each instantiation of the alias is
    // interchangeable with the corresponding instantiation of the aliased
    // type". Every arity-1 builtin head in the modeled family (`Array`,
    // `Set`, `Stack`, `Queue`, `LinkedList`, `SortedSet`, and the collection
    // interfaces) was unusable through an alias; the arity-2 maps
    // (`Dictionary`, `SortedDictionary`) only APPEARED to work because they
    // sit outside that family and fell through to the conservative accept —
    // they were never alias-transparent either, merely un-rejected.
    //
    // Placed AFTER `declaration` and `importedName` — through an alias copy
    // both already carry the UNDERLYING type's own name, so those legs are
    // already alias-transparent — and BEFORE the astNode read, the only leg
    // the alias restore corrupts. Non-alias refs leave `aliasResolvedHead`
    // empty and are unaffected, and the qualified-name `parts.back()`
    // (last-component) behaviour this function deliberately keeps, unlike
    // `headName()`'s `parts[0]`, is preserved for them.
    if (!ref->aliasResolvedHead.empty()) return ref->aliasResolvedHead;
    if (ref->astNode && ref->astNode->name &&
        !ref->astNode->name->parts.empty())
        return ref->astNode->name->parts.back();
    return {};
}
// hierarchy without requiring real TypeDecl nodes.
//
// The hierarchy (per stdlib §4.1–§4.12):
//   Array<T>      : IList<T>
//   OwningArray<T> : IOwningList<T>
//   FrozenArray<T> : IEnumerable<T>
//   Range<T>       : IEnumerable<T>
//   Set<T>        : ICollection<T>            // mutating borrowing collection
//   LinkedList<T> : ICollection<T>            // mutating borrowing collection
//   Stack<T>      : IReadOnlyCollection<T>    // read-only chain only (v0.165)
//   Queue<T>      : IReadOnlyCollection<T>
//   SortedSet<T>  : IReadOnlyCollection<T>
//   IList<T>      : IReadOnlyList<T>, ICollection<T>
//   IOwningList<T> : IReadOnlyList<T>, IOwningCollection<T>
//   IReadOnlyList<T>       : IReadOnlyCollection<T>
//   ICollection<T>         : IReadOnlyCollection<T>
//   IOwningCollection<T>    : IReadOnlyCollection<T>
//   IReadOnlyCollection<T> : IEnumerable<T>
//   IEnumerable<T>         : (root)
// The concrete borrowing collections above each implement exactly one collection
// interface (transitively closed here to the full read-only chain), so their
// complete generic-interface set is captured — this is what
// builtinBorrowingCollectionFullyModeled relies on to let the top-level variance
// check REJECT a proven non-subtype instead of conservatively accepting (#1851).
// The owning concretes (Owning*) map to IReadOnlyCollection conservatively (their
// owning graph — IOwningEnumerable etc. — is not modeled), so they are NOT
// treated as fully modeled and retain conservative-accept.
//
// Only arity-1 types are covered here; types without standard covariant element
// chains (e.g. Dictionary<K,V>) are not in scope for this helper.
inline std::vector<const TypeRef*> builtinInterfaceAncestors(
    std::string_view name, const std::vector<TypeRef*>& genericArgs,
    std::vector<std::unique_ptr<TypeRef>>& owned) {
    // Helper to create a synthesised null-declaration Named TypeRef.
    // ifaceName MUST be a string-literal (static lifetime) so the
    // string_view importedName field remains valid indefinitely.
    auto makeBuiltin = [&](std::string_view ifaceName,
                           std::vector<TypeRef*> args) -> TypeRef* {
        auto ref = std::make_unique<TypeRef>();
        ref->kind = TypeRef::Kind::Named;
        ref->declaration = nullptr;
        ref->importedName = ifaceName;  // caller supplies a string literal
        ref->genericArgs = std::move(args);
        TypeRef* raw = ref.get();
        owned.push_back(std::move(ref));
        return raw;
    };
    // Only handle arity-1 types with a meaningful interface chain.
    if (genericArgs.size() != 1) return {};
    TypeRef* T = genericArgs[0];
    if (!T) return {};
    // Build the transitive ancestor list in breadth-first order (topmost
    // interfaces first so the BFS in extensionTargetMatchesReceiverOrBase
    // encounters them in the most-specific → least-specific direction).
    std::vector<const TypeRef*> ancestors;
    // Tier 1 parents of the concrete collection types.
    if (name == "Array")
        ancestors.push_back(makeBuiltin("IList", {T}));
    else if (name == "OwningArray")
        ancestors.push_back(makeBuiltin("IOwningList", {T}));
    else if (name == "FrozenArray")
        // fix #8103 (§26.5 rule 2, stdlib §4.19, v0.355): `FrozenArray<T>`
        // is declared `public class FrozenArray<T>: IEnumerable<T>`, so
        // `IEnumerable<T>` is its (only) implemented interface.
        ancestors.push_back(makeBuiltin("IEnumerable", {T}));
    else if (name == "Range")
        // fix #8393 (§26.5 rule 2, stdlib §9.8, v0.357): `Range<T>` is
        // declared `public record Range<T>: IEnumerable<T>` (stdlib §9.8),
        // so `IEnumerable<T>` is its (only) implemented interface — the
        // same shape as the #8103 `FrozenArray<T>` row.
        ancestors.push_back(makeBuiltin("IEnumerable", {T}));
    else if (name == "Set" || name == "LinkedList")
        // #1851: Set<T> and LinkedList<T> implement ICollection<T> (mutating
        // borrowing collections, stdlib §4.6/§4.9), not merely
        // IReadOnlyCollection<T>. The read-only chain stays reachable
        // transitively (ICollection → IReadOnlyCollection → IEnumerable).
        ancestors.push_back(makeBuiltin("ICollection", {T}));
    else if (name == "Stack" || name == "OwningStack" ||
             name == "Queue" || name == "OwningQueue" ||
             name == "OwningSet" || name == "SortedSet" ||
             name == "OwningLinkedList")
        // Stack/Queue/SortedSet conform only to the read-only chain (v0.165,
        // #1354). The Owning* concretes map here conservatively (see header).
        ancestors.push_back(makeBuiltin("IReadOnlyCollection", {T}));
    else if (name == "SharedEventSource")
        // fix #4793 / §6.5 (v0.307): SharedEventSource<T> implements
        // ISharedEvent<T> and ONLY ISharedEvent<T>. Registering the
        // ISharedEvent<T> ancestor admits the legal upcast (the
        // ISharedEvent<S>.Subscribe capture-free signature). The IEvent<S>
        // entry is deliberately OMITTED — §6.5 names it: an upcast would
        // launder the capture-free requirement through IEvent<S>.Subscribe's
        // capture-admitting `scoped` signature. The omission, combined with
        // SharedEventSource's registration in
        // `builtinBorrowingCollectionFullyModeled` below, makes a missing
        // IEvent<S> ancestor a provable non-conformance and the §5.1
        // assignment is REJECTed, rather than the variant search's
        // conservative-accept fallback.
        ancestors.push_back(makeBuiltin("ISharedEvent", {T}));
    else if (name == "IList")
        ancestors.push_back(makeBuiltin("IReadOnlyList", {T})),
        ancestors.push_back(makeBuiltin("ICollection", {T}));
    else if (name == "IOwningList")
        ancestors.push_back(makeBuiltin("IReadOnlyList", {T})),
        ancestors.push_back(makeBuiltin("IOwningCollection", {T}));
    else if (name == "ICollection")
        ancestors.push_back(makeBuiltin("IReadOnlyCollection", {T}));
    else if (name == "IOwningCollection")
        ancestors.push_back(makeBuiltin("IReadOnlyCollection", {T}));
    else if (name == "IReadOnlyList")
        ancestors.push_back(makeBuiltin("IReadOnlyCollection", {T}));
    else if (name == "IReadOnlyCollection")
        ancestors.push_back(makeBuiltin("IEnumerable", {T}));
    // For a name we don't recognise at this level, return empty (no hierarchy
    // known) — this is conservative: no false-positive extension matches.
    if (ancestors.empty()) return {};
    // Recursively expand each ancestor's own ancestors so the returned list is
    // the full transitive closure.
    std::vector<const TypeRef*> result;
    std::vector<std::unique_ptr<TypeRef>> scratch;
    std::vector<const TypeRef*> frontier(ancestors.begin(), ancestors.end());
    std::size_t fi = 0;
    while (fi < frontier.size()) {
        const TypeRef* anc = frontier[fi++];
        result.push_back(anc);
        if (anc->kind == TypeRef::Kind::Named && !anc->importedName.empty()) {
            std::vector<std::unique_ptr<TypeRef>> childOwned;
            auto children = builtinInterfaceAncestors(
                anc->importedName, anc->genericArgs, childOwned);
            for (auto& u : childOwned) owned.push_back(std::move(u));
            for (const TypeRef* c : children) frontier.push_back(c);
        }
    }
    return result;
}

// §26.5 (#714): convenience wrapper — same as above but derives name and args
// from the receiver TypeRef directly (must be a null-decl Named builtin).
inline std::vector<const TypeRef*> builtinInterfaceAncestorsOf(
    const TypeRef* recv,
    std::vector<std::unique_ptr<TypeRef>>& owned) {
    if (!recv || recv->kind != TypeRef::Kind::Named) return {};
    // fix #8103 (§26.5 rule 2, v0.355): the `builtinClassSym` stub a static
    // factory or built-in call result carries (`FrozenArray<int>.From(a)`)
    // is the same built-in type as the declaration-less annotation shape.
    if (recv->declaration && isUserDeclaredTypeSym(recv->declaration))
        return {};
    std::string_view name = namedTypeSimpleName(recv);
    if (name.empty()) return {};
    return builtinInterfaceAncestors(name,
        const_cast<std::vector<TypeRef*>&>(recv->genericArgs), owned);
}
}  // namespace bezel::sema
