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

}  // namespace bezel::sema
