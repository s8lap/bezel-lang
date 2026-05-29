#pragma once

// Ownership-state lattice per §28.3.
//
// Each owned-class-typed local lives in one of these states at every
// program point. The lattice is intentionally NON-COMPLETE — the
// branch-join operation rejects mismatched merges instead of widening
// to the weaker state, because silently demoting Owned → Borrowed at
// a join would leak the resource on the owned-branch path.
//
// Spec authority: §28.3 "Local variables, ownership, and borrows" (the
// "branch joins" subsection that documents the asymmetric merge rule).
//
// Slice O0 introduces the data structures only; later O-slices add the
// dataflow walk, producer table, discharge analysis, branch joins,
// move semantics, and exception-path discharge.

#include <cstdint>
#include <optional>
#include <string_view>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"

namespace bezel::ownership {

enum class OwnershipState {
    Uninitialized,   // var x: T;  not yet assigned
    Owned,           // discharge obligation lives here
    Borrowed,        // borrow source attached
    MovedFrom,       // post-move on an owned slot; must be re-assigned
    Disposed,        // post explicit .Dispose(); the local is dead
    PolyNull,        // §28.3: `var x: T? := null;` until first non-null assign
    Poisoned,        // a prior error referenced this local; suppress cascades
};

// Where a borrowed value's source is, expressed structurally enough to
// answer "is the borrow's lifetime <= the source's lifetime?". For slice
// O0 we just hold the source Symbol*; sema's ScopeId on that symbol is
// the lifetime extent (a borrow with a larger holder-scope than its
// source's scope is illegal).
struct BorrowSource {
    sema::Symbol* sym = nullptr;
};

struct LocalOwnership {
    OwnershipState state = OwnershipState::Uninitialized;
    // Populated when state == Borrowed.
    std::optional<BorrowSource> source;
    // For Owned: a back-pointer to the AST node where the obligation
    // was created (the `:= EXPR` site), used for diagnostic notes.
    const ast::Node* obligationOrigin = nullptr;
    // True for `using var` — discharge is implicit at scope exit.
    bool implicitDischarge = false;
};

// Spelled state for diagnostics + tests.
inline std::string_view ownershipStateName(OwnershipState s) {
    switch (s) {
        case OwnershipState::Uninitialized: return "uninitialized";
        case OwnershipState::Owned:         return "owned";
        case OwnershipState::Borrowed:      return "borrowed";
        case OwnershipState::MovedFrom:     return "moved-from";
        case OwnershipState::Disposed:      return "disposed";
        case OwnershipState::PolyNull:      return "poly-null";
        case OwnershipState::Poisoned:      return "poisoned";
    }
    return "?";
}

} // namespace bezel::ownership
