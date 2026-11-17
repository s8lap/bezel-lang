#pragma once

// §9.8 (v0.357, issue #8391): the ELEMENT type of a standard SEQUENCE
// collection component, or nullptr when `ty` is not one.
//
// §9.8's admissible-component list reads: "a standard sequence collection
// (`Array<T>`, `OwningArray<T>`, `LinkedList<T>`, etc., and inline arrays
// `[N]T`) whose element type is admissible as a serialized component (encoded
// with `BeginArray`/`EndArray`)", and — of the admissible rows — "the heap
// collections (sequence and dictionary alike, which are class types, §3.1) are
// admissible in **class** components only".
//
// WHY THIS PREDICATE IS SHARED, AND WHY IT LIVES HERE. Two places must
// agree on which shapes are sequence collections:
//
//   * sema's §9.8 eligibility check (`isSerializationEligibleType`) — which
//     component types the conformance gate admits; and
//   * codegen's §9.8 wire emitters (`emitSerializeField` /
//     `emitDeserializeField`) — which shapes it can actually WRITE and READ
//     back.
//
// Spelling the shape twice is how the eligibility gate would admit a shape the
// emitter cannot write — the duplicated-predicate failure mode
// `sema::serializationComponentTypeIsEmittable`'s comment describes. So the
// shape test lives HERE, and both readers call it.
//
// It lives in its OWN header rather than in `sema/Symbol.hpp` because
// `Symbol.hpp` is included by ~150 translation units (every codegen TU
// reaches it through `codegen/LlvmBackend_impl.hpp`), so a declaration added
// there rebuilds the whole test suite for a two-reader predicate. The
// precedent is `sema/SpanFamily.hpp`, the same shape for the same reason.
//
// THE HEADS THIS ANSWERS FOR. The two §9.8 names that share ONE runtime
// representation (`bz_array_t*`) and ONE element order (index 0 is the first
// element): `Array<T>` (stdlib §4.4, the borrowing array) and `OwningArray<T>`
// (stdlib §4.5, the owning array). §9.8's "etc." also reaches the
// list-backed (`LinkedList<T>`, stdlib §4.6), stack/queue-backed (§4.11) and
// dictionary-backed (§4.8) families, whose wire form and construction are NOT
// the `bz_array_t*` shape this predicate answers for — see issue #8511.
//
// The inline-array row (`[N]T`) is NOT answered here: it is a VALUE type
// (§15.1), stored inline in the enclosing struct, and §9.8 admits it as a
// RECORD component too — a different rule, handled by the eligibility
// predicate's own `Kind::InlineArray` branch.

#include <string_view>

#include "sema/TypeRef.hpp"

namespace bezel::sema {

// The ELEMENT type of a standard sequence collection, or nullptr when `ty` is
// not one.
//
// A builtin collection type has NO `declaration` (there is no user TypeDecl
// for it) and carries its name in `astNode` (locally) or `importedName`
// (across a module boundary); `headName()` is the ONE reader that sees
// through a type alias (§27.8) and covers both sources.
inline const TypeRef* serializationSequenceCollectionElement(const TypeRef* ty) {
    if (!ty) return nullptr;
    if (ty->kind != TypeRef::Kind::Named) return nullptr;
    if (ty->declaration) return nullptr;  // a user type, not a builtin
    std::string_view head = ty->headName();
    if (head != "Array" && head != "OwningArray") return nullptr;
    if (ty->genericArgs.size() != 1 || !ty->genericArgs[0]) return nullptr;
    return ty->genericArgs[0];
}

}  // namespace bezel::sema
