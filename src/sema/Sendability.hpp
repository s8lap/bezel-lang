#pragma once

// §30.14 / §3.6 (#600, #2050): the sendability derived-type property.
//
// A type is *sendable* when a value of it may cross an execution-domain
// boundary — as a `Domain.Spawn` argument or result, or as a `Channel<T>`
// element. The rule is structural: value types are sendable iff they are
// not borrow-bearing and not move-only; `string` is sendable; function
// values are never sendable; class types are sendable iff they are
// "transfer-closed" (sealed, with every reachable field either a sendable
// value, a `string`, or an `owned` reference to a sendable class). The
// full §30.14 table — including the compiler-known standard-library
// overrides (Mutex/Semaphore/Atomic/Channel/FrozenArray/Shared<T> of a
// synchronized payload, etc.) — is implemented in Sendability.cpp.
//
// This header exposes the computation as a free function so that BOTH
// the expression type-checker (the `ExprTC` worker — call-site checks at
// `Domain.Spawn` / `new Channel<T>()` / `new ChannelSender<T>()`) and
// the type resolver (the `Resolver` worker — `where T: sendable` derived-
// property constraint satisfaction at the instantiation site, §14.2
// "Derived-property constraints", v0.201) consult ONE implementation of
// §30.14. The constraint "adds no second definition of the property"
// (§14.2): both routes feed the same computation.
//
// Coinductive recursion over a class hierarchy is handled via the
// `inProgress` set — pass an empty set on a top-level call (or use the
// single-arg overload) and the implementation threads it through the
// recursive sub-calls.

#include <string>
#include <unordered_set>

#include "sema/SemaContext.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// §30.14 sendability computation. Returns true iff `rt` satisfies the
// §30.14 sendable-type table. `inProgress` carries the coinductive
// assumption set across recursive sub-calls (assume the types under test
// sendable, then check the rules); pass an empty set on the initial call.
bool isSendableType(const TypeRef* rt, SemaContext& ctx,
                    std::unordered_set<const TypeSymbol*>& inProgress);

// Convenience top-level entry: threads a fresh empty `inProgress` set.
inline bool isSendableType(const TypeRef* rt, SemaContext& ctx) {
    std::unordered_set<const TypeSymbol*> ip;
    return isSendableType(rt, ctx, ip);
}

// #5305 / §30.14 (v0.313) / §30.15: the AGGREGATE channel-element set.
//
// A `Channel<T>` item rides one ptr-sized word (§30.15's `void*` cell). A
// scalar value type (primitive, enum, `double`, `char`) rides it directly; an
// AGGREGATE value type — one whose §2.8.3 lowering is an LLVM struct or array,
// which is every sendable value type with no ≤64-bit encoding — has no such
// encoding, so the compiler carries it as a POINTER to a fresh heap cell
// holding a copy of the value — §30.14's value-type bullet verbatim:
// "A sendable value crosses as a copy". The set is declared HERE, once, so
// that every site
// which must agree on it by construction does. All of them are codegen, and
// all of them are keyed on this predicate rather than on the lowered LLVM
// type, so the ENCODER and each DECODER cannot drift:
//
//   - the item-word codec `encodeChannelItemWord` / `decodeChannelItemWord`
//     (LlvmBackend_Calls3.cpp) — `Send`, `SendAsync`, `TryReceive`;
//   - `channelReceiveHasAggregateElement` (LlvmBackend_Async.cpp) — the
//     `await ch.Receive()` result;
//   - `channelDrainHasAggregateElement` (LlvmBackend_Async.cpp) — the
//     `await for … in ch.ReceiveAll()` loop variable;
//   - the `new Channel<T>()` element disposer (LlvmBackend_Exprs2.cpp) — an
//     item still buffered at disposal is a CELL, so the disposer frees the
//     cell rather than the value.
//
// The members (a sendable record, a choice — §35.12 v0.313 for the latter —
// an inline array `[N]T` §15.1, the `decimal` primitive §4.4, and the
// nullable VALUE forms of each, which §7.11 lowers to a tagged
// `{i8 hasValue; T}` struct) are enumerated with their per-member evidence in
// the definition, Sendability.cpp.
//
// Deliberately NOT keyed on the lowered LLVM type (which would also catch the
// `Span`/`Range`/`Guid` struct builtins): those carry borrows and are not
// sendable (§30.14), so they are not legal `Channel<T>` elements.
bool isAggregateChannelElement(const TypeRef* rt);

// §14.2 (v0.201 sharpened §3.6 rule 4, #2059): given a type that has just
// FAILED the §30.14 sendability check, walk its structure to identify the
// FIRST structural reason for failure and return a component-path fragment
// naming the offending field/element + the failing §30.14 rule, shaped to
// APPEND to the existing flat constraint-failure diagnostic, e.g.
//   ": field `f` of type `Open` is not owned (§30.14 rule 3)"
//   ": element type `Open` is not sendable (§30.14 value-type rule)"
//   ": class is neither `sealed` nor `closed` (§30.14 rule 1)"
//
// Returns empty when the walk is ambiguous (generic-parameter reference,
// stdlib compiler-known override, function-typed value at the root, an
// unresolved / Error TypeRef, or any other verdict the structural walk
// cannot confidently pin to a single field-or-element chain) — callers
// then keep the existing flat message rather than over-promising detail.
// The depth-bounded walk terminates at a non-recursing leaf (typically
// the first field whose own type is non-sendable for a non-structural
// reason; nested sealed-class chains recurse one level deep).
//
// Used by BOTH constraint-check sites — the type-instantiation site
// (Resolver::checkOneConstraint in TypeResolution_Checks.cpp) and the
// method-call site (checkMethodGenericConstraints in
// ExprTypecheck_Calls.cpp) — per the #2059 deferral-from-#2050 note:
// "the component-path machinery is shared by both check sites".
std::string sendableFailureComponentPath(const TypeRef* rt, SemaContext& ctx);

}  // namespace bezel::sema
