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
    // fix #4236 / §28.3 "Ownership state of locals", §28.6 "Who may call
    // Dispose()" (v0.298): a DEDICATED, narrower sibling of `source` above,
    // consumed ONLY by checkDisposeReceiverOwnership. Populated at THREE
    // sites, all in Ownership.cpp, all applying the same narrow bare-
    // IDENTIFIER-of-a-still-Owned-tracked-local test: `handleLocalDecl` (a
    // plain, non-`using`, non-`owned`-marked local declaration), the
    // LocalConstDecl arm (a `const`, fix #4236 send-back — §21.1/#3687
    // already established a `const` is tracked exactly as a plain unmarked
    // `var` alias for `source`; this is that statement for
    // `disposeAliasSource`), and `handleAssignment`, which RE-DERIVES
    // (set-or-cleared) it on every plain reassignment of the local so a
    // rebind away from an alias (`b := a; b := c;`) does not leave a stale
    // alias-source and a rebind onto one is newly recognized — it is NOT
    // immune to later state changes, it is recomputed alongside `source` at
    // every one of them. `joinInto` (Ownership_Checks.cpp, fix #4236
    // send-back) additionally OR-merges it across a branch join that
    // converges to the same state, so which arm is listed first cannot
    // change whether a post-join alias is recognized. Deliberately NOT
    // reused from `source`
    // (borrowSourceForExpr's Shape 1): that field's populate gate
    // (returnedExprMayBorrow -> isBorrowableType) answers FALSE for every
    // compiler-builtin collection (Array/Dictionary/Set/LinkedList/...) — a
    // Named TypeRef with a null `declaration` is the same shape
    // isBorrowableType uses for an unresolved generic parameter, and its
    // null-declaration branch recognizes only the builtin-exception
    // hierarchy positively — so `source` is never set for a collection-typed
    // alias local at all (measured: #4236's repro). Widening `source`
    // itself would also change every OTHER consumer of it (the borrow-
    // escape-on-return/-store checks), an unvalidated, unrelated blast
    // radius this fix does not take on; a separate field keeps the change
    // exactly as narrow as the bug it fixes.
    sema::Symbol* disposeAliasSource = nullptr;
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

    // fix #4246 (§28.3 / §28.6, v0.298): true once an explicit `.Dispose()`
    // call, for THIS local's CURRENT value-epoch, was classified as
    // reclaim-eligible by `markExplicitDisposeReclaimIfReassignedBorrow`
    // (i.e. `state == Owned` at that call, on a borrow-initialized local
    // that became owned via reassignment — the shape `unwindOwnedLocalClass`
    // cannot see because it only reads the DECLARATION). Sticky ACROSS the
    // Owned -> Disposed transition that same call causes, so a REPEATED
    // `.Dispose()` on the same value-epoch (§28.2 idempotence) is recognised
    // too: codegen must return the same slot on the repeat call as it did on
    // the reclaiming one, or the repeat call's dispatch-only fallback reads
    // through the pointer the reclaim already nulled — a null-deref SIGSEGV
    // (measured: `y := new Res(1); y.Dispose(); y.Dispose();` after a
    // borrow-init `var y := x;`, before this field existed).
    //
    // RESET on every plain `:=` reassignment (`handleAssignment`, alongside
    // `maybeConsumed`/`disposedOnSomePath`/`awaitSpent`) — load-bearing:
    // the slot's CONTENT changes at a reassignment, so a stale `true` would
    // make a LATER, unrelated dispose on a freshly-reassigned genuine borrow
    // also reclaim, double-freeing a value this local never owned. Because
    // the reset always runs before `state` can become Owned again, a
    // following reassignment to another owned producer re-derives this flag
    // from scratch at its own next explicit dispose, and the two owned
    // "epochs" cannot bleed into each other.
    bool explicitDisposeReclaimedSlot = false;

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

    // §28.3 (v0.180, #1626): maybe-consumed — the maybe-consumed lattice
    // state, realized as a flag ORTHOGONAL to the Owned state. Set at a
    // control-flow join where one incoming edge holds the local Owned and the
    // other has MOVED FROM it (MovedFrom via a move / `owned`-transfer / owned
    // argument) — ownership left the local on that branch, the object may be
    // freed by its new owner, and any post-join use is a use-after-free.
    //
    //   "A join of owned with moved-from ... yields maybe-consumed (v0.180):
    //    the variable may not be read, borrowed from, or consumed after the
    //    join, and may not flow around a loop back edge into any use;
    //    scope-end auto-drop applies per-path (disposal runs only on the
    //    still-owned path)."
    //
    // Rework (#1626 v2, reconciling §28.3 with §28.2 / #601): the marker is
    // NOT set for the Owned ⊓ DISPOSED split, even though §28.3's literal
    // "(or explicitly disposed)" clause groups it with moved-from. An explicit
    // Dispose() is idempotent — §28.2: "a further Dispose() call remains a
    // no-op, not an error, in both modes" — and the local still OWNS the
    // reference on both branches (Disposed is a logical state, not a transfer;
    // the object is freed only at the per-path scope-end auto-drop). So the
    // Owned ⊓ Disposed join resolves to plain Owned with no marker: the
    // post-join Dispose is the proper discharge on the owned path and an
    // idempotent no-op on the disposed path (#601's one-branch discharge
    // pattern), and reads fall to the runtime disposed-flag backstop (§28.2
    // checked mode) just as for any non-dominating use-after-dispose. Setting
    // the marker there would over-reject that valid program. The marker is the
    // move/transfer use-after-free danger ONLY.
    //
    // The state stays Owned (NOT a new enum lattice value) so that all the
    // discharge machinery — the end-of-scope leak check / affine auto-drop
    // (§28.3), the exception-path discharge, and #338's per-path obligation
    // retention — behave EXACTLY as for a plain Owned local: the still-owned
    // branch's value must still be disposed at scope end on that path, which
    // the runtime disposed-flag/null check makes a no-op on the moved-from
    // path. A fresh enum value would have required touching every
    // `state == Owned` discharge site (each a potential soundness gap or
    // over-rejection if mis-classified); the flag leaves that machinery
    // untouched by construction. It is consulted only at the use sites —
    // checkRead (read/borrow), markMovedByName (consume/move), and
    // markDisposedByName (explicit Dispose) — each of which rejects a
    // maybe-consumed local with a diagnostic pointing at the offending use
    // (the spec's "diagnostics should point at the offending use, not the
    // declaration"). Reassignment to a fresh owned value clears the flag
    // (the local is then definitely owned again). // per §28.3, §28.2
    bool maybeConsumed = false;

    // #1869 (§30.5 / Appendix C.6): set on a Disposed record produced by a
    // Pattern B `await` consume of a plain owned `OwningTask<T>` local
    // (recordAwaitOperandConsume). Unlike an idempotent `.Dispose()`, a Pattern B
    // await is a single-consumer SPEND — the result is moved out, a second await
    // raises InvalidOperationException, and a `move`/re-await moves out an
    // already-spent task. So the `Owned ⊓ Disposed` BRANCH-join benign rule
    // (#601 idempotent-Dispose, above) does NOT apply when the Disposed side is
    // an await-spend: joinInto upgrades that one-branch spend to `maybeConsumed`
    // (branch merges only — loop back-edges keep the plain-Owned/suspend-local
    // handling unchanged). // per §30.5, Appendix C.6, §28.3
    bool awaitSpent = false;

    // §28.3 (#1883): set on the Owned record resulting from an Owned ⊓
    // Disposed join (the post-#1626 rework Disposed-half behavior). Distinct
    // from `maybeConsumed` (which is the MOVED-FROM split's use-after-free
    // danger): the Disposed split is BENIGN for an explicit Dispose — §28.2
    // idempotence — but a READ after the join would operate on a disposed
    // object on the disposed-branch path (use-after-dispose, §28.2). The
    // markDisposedByName carve-out (#601: "a further Dispose() call remains
    // a no-op, not an error, in both modes") keeps the post-join Dispose
    // accepted, so this flag is consulted by checkRead / markMovedByName
    // only — NOT by markDisposedByName (which would otherwise over-reject
    // #601's one-branch discharge pattern). joinInto sets it on the
    // surviving Owned record (reassignment clears it via handleAssignment,
    // exactly like maybeConsumed — the local is then definitely owned again).
    // // per §28.3, §28.2
    bool disposedOnSomePath = false;
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
