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
    // §28.2 v0.131: location of an explicit `.Dispose()` call that put this
    // local into the Disposed state. Populated by markDisposedByName; used
    // to attach a Note pointing at the dispose site on the use-after-dispose
    // warning.  Zero-initialised (invalid) when the local was never explicitly
    // disposed (state transitions from Owned → Disposed only through an
    // explicit call, so the field is always valid when state == Disposed from
    // an explicit call; the only other path to Disposed is markDisposedByName).
    SourceLocation explicitDisposeLoc;
    // True when the Disposed state was set by an explicit x.Dispose() call (as
    // opposed to the using-var implicit scope-end disposal, where we never call
    // markDisposedByName from the checker because the implicit dispose doesn't
    // pass through the ownership-checker code path that reads the state).
    bool explicitlyDisposed = false;

    // Async-readiness 3c: SuspendLocal annotation (§30.5 / §30.9). True when
    // this owned local's live range crosses at least one `await` suspend
    // point in an async method — i.e. it is still live when the method
    // suspends. A suspend-local must be stored in the state-machine frame and
    // disposed on EVERY completion path: normal return, fault/exception,
    // abandonment, and cancellation (§30.5 task lifecycle; §30.9 fault). The
    // model deliberately keeps this as a flag on LocalOwnership rather than a
    // new OwnershipState lattice value: the disposal obligations of a
    // suspend-local are identical to an ordinary owned local's (the lattice is
    // unchanged), and the "disposed on every completion path" property is
    // modelled by the synthetic completion edges (CFGBlock::completionSuccs)
    // that give the abandonment/cancellation exits the same frame-disposal
    // treatment as the §28.3 exception-path discharge. This flag is the
    // recorded classification the future async-codegen milestone consumes to
    // decide which locals live in the heap-allocated frame vs the activation
    // record; for the ownership pass it is informational (the diagnostics flow
    // from the unchanged lattice + the synthetic/exception completion edges).
    // An activation-local (live range does NOT cross an await) leaves this
    // false and behaves exactly as a synchronous owned local. // per §30.5
    bool suspendLocal = false;
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
