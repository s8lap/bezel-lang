#pragma once

// stdlib §3.3 / §4.14 / §4.15 (v0.355): the SPAN VIEW family — the value
// types that view a borrowed range of a string, an `Array<T>` or an
// `OwningBuffer<T>`:
//
//   * `ReadOnlyStringSpan` (stdlib §3.3) — a view over a `string`;
//   * `Span<T>` / `ReadOnlySpan<T>` (stdlib §4.14) — a view over an
//     `OwningBuffer<T>`;
//   * `ArraySpan<T>` / `ReadOnlyArraySpan<T>` (stdlib §4.15) — a view over an
//     `Array<T>`.
//
// WHY THIS PREDICATE IS SHARED. The five heads are ONE family in every place
// that matters:
//
//   * they all lower to the SAME `{ptr source, i32 start, i32 length}` value
//     struct (codegen's `arraySpanStructType`), so a lowering that reads a
//     span's fields reads one shape;
//   * a NULLABLE one of them is a nullable VALUE type: it lowers to the
//     tagged `{i8 hasValue; {ptr,i32,i32}}` carrier
//     (codegen's `isNullableValueStructBuiltinTy` / `nullableValueStructTy`),
//     so a §7.4-narrowed read of one reaches codegen as the CARRIER while
//     every span consumer wants the PAYLOAD; and
//   * §7.4's member-access gate and §7.4's indexer gate are the two places
//     that admit a narrowed nullable receiver, and both must recognise the
//     family to publish that read (fix #8211).
//
// Spelling the family twice — once per gate — is how the two gates would
// drift apart, so the head test lives HERE.
//
// NOT IN SCOPE. `IReadOnlySpan<T>` (stdlib §4.16) is the INTERFACE over the
// concrete span views, not a value struct: it is a reference type, so it
// carries no carrier and has no payload to unwrap.

#include <string_view>

#include "sema/TypeRef.hpp"

namespace bezel::sema {

// The five span-view head names (stdlib §3.3 / §4.14 / §4.15).
inline bool isSpanFamilyHead(std::string_view head) {
    return head == "Span" || head == "ReadOnlySpan" ||
           head == "ArraySpan" || head == "ReadOnlyArraySpan" ||
           head == "ReadOnlyStringSpan";
}

// Is `rt` one of the span views? Reads the head the way every builtin
// identity check in this compiler reads it (TypeRef::headName — the
// alias-transparent read, §27.8), plus the builtin-stub declaration name for
// the shape that carries a TypeSymbol.
inline bool isSpanFamilyTypeRef(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named) return false;
    if (rt->declaration) return isSpanFamilyHead(rt->declaration->name);
    return isSpanFamilyHead(rt->headName());
}

}  // namespace bezel::sema
