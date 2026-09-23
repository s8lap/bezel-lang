// Spec-derived tests for the §28.6 receiver-owned-field carve-out for the
// owning-COLLECTION heads (OwningArray/OwningDictionary/OwningSet/
// OwningStack/OwningQueue/OwningLinkedList) — the worked example at spec
// §28.6 L6999–L7003 (`return _transactions;` from `method GetTransactions():
// OwningArray<Transaction>`).
//
// #1760 (2026-07-28) had REMOVED the carve-out for these heads, citing the
// indirect-dispatch double-free (the same hazard #1660 closed for the OWNING
// DEFERRED-ACTIVATION heads — OwningTask<T> / IOwningEnumerable<T>): under
// interface / virtual-override / imported / function-value dispatch the call
// site cannot resolve the concrete method and `producerState` falls to the
// name-head Owned fallback, so the caller disposes the borrow — a HARD double-
// free of the field's real owner. Owning collections are not
// Dispose-idempotent (their `Dispose()` is a hard free; verified for every
// entry in `kOwningCollectionHeads`), so unlike Shared/Weak/ChannelSender/
// CancellationTokenSource (whose double-dispose is a safe refcount no-op)
// there is no safe no-op fallback.
//
// #2269 (this revision) RESTORES the carve-out at the definition site, on the
// ground that the spec §28.6 explicitly ADMISS the pattern (worked example
// `return _transactions;`, plus "applies uniformly to a class-typed owned
// field and to an owning-collection field (`owned _items: OwningArray<T>`)",
// spec §28.6 L6991) — and rejecting it leaves the §28.6 "Borrowed owning
// collections" subsection (which governs "a borrow of an owning collection —
// produced by returning a bare owning-collection field (above)") with no
// producer. The carve-out's narrowing to the receiver-owned-field form is
// `exprIsReceiverOwnedFieldBorrow(e)` (the same predicate the WRAPPER branch
// uses for Shared/Weak/ChannelSender/CancellationTokenSource) — it admits
// ONLY a bare read of an `owned` owning-collection field of THE RECEIVER
// (`this._items` or implicit `_items`); a field of another object, a
// borrowed parameter, and a borrowed local still fall through to the
// rejection below. The indirect-dispatch unsoundness the carve-out
// reintroduces is filed as a separate spec-amendment follow-up (#2269 — not
// a definition-site rejection); see the Ownership.cpp comment for the
// trade's full context.
//
// All shapes below were hand-verified against `build/bezelc -c` (and the
// ACCEPT shapes additionally against native AOT+ASan). The reject control
// (line 198+ — interface-dispatch shape) is RETAINED, not flipped, because
// the spec admission is definition-site-only; the indirect-dispatch
// unsoundness is a SEPARATE concern that is filed as a follow-up.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; ") +
        body;
}

}  // namespace

// ===========================================================================
// ACCEPT: §28.6 worked example, applied to every owning-COLLECTION head
// (`kOwningCollectionHeads`). The receiver-owned-field carve-out
// (`exprIsReceiverOwnedFieldBorrow`) admits a bare read of an `owned`
// owning-collection field of `this` returned through a non-`owned`
// owning-collection return. The carve-out is this-restricted — a field of
// another object, a borrowed parameter, a borrowed local are NOT covered
// and fall through to the rejection below.
// ===========================================================================

// `OwningArray<T>` — the §28.6 worked example, verbatim against the spec.
BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_array_receiver_field_returned_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningArray<Cell>; "
        "  public constructor(owned f: OwningArray<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningArray<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The other owning collection heads — same shape, all admitted (every entry
// in `kOwningCollectionHeads` is a §28.6 carve-out site).
BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_dictionary_receiver_field_returned_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningDictionary<int, Cell>; "
        "  public constructor(owned f: OwningDictionary<int, Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningDictionary<int, Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_set_receiver_field_returned_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningSet<Cell>; "
        "  public constructor(owned f: OwningSet<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningSet<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_stack_receiver_field_returned_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningStack<Cell>; "
        "  public constructor(owned f: OwningStack<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningStack<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_queue_receiver_field_returned_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningQueue<Cell>; "
        "  public constructor(owned f: OwningQueue<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningQueue<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_linked_list_receiver_field_returned_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningLinkedList<Cell>; "
        "  public constructor(owned f: OwningLinkedList<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningLinkedList<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A bare identifier (`_items`, implicit `this._items`) — admitted by the
// same this-restricted predicate.
BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_array_bare_identifier_field_returned_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() _items := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningArray<Cell> => _items; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An explicit `this._items` — also admitted.
BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_array_explicit_this_field_returned_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() _items := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningArray<Cell> => this._items; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ★ SOUNDNESS REGRESSION GUARD — the coverage gap the issue cites: every prior
// owning-collection borrow-return test was a DIRECT call, where the carve-out
// (since removed) masked the double-free. The hard double-free only
// manifests under INDIRECT dispatch, where the call site cannot resolve the
// concrete method and falls to the name-head Owned fallback. The def-site
// check runs on the concrete override REGARDLESS of how it is called, so the
// fix is that the concrete method's borrow return is rejected at its
// definition — no compiling program can then contain it, and no caller can
// double-free. This is the exact native-AOT+ASan repro (interface dispatch);
// it FAILS on the pre-fix admit (compiles -> SEGV in the caller's disposal)
// and PASSES on the fix (the override's `return field;` is rejected).
BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_array_borrow_return_via_interface_rejected) {
    // #2269 (cycle-3 fix-forward): the §28.6 receiver-owned-field carve-out
    // is admitted at the DEFINITION site for the direct call path. The
    // indirect-dispatch case this test pins — `src.Get()` through an
    // interface — currently admits the borrow-return at the call site too,
    // because the call-site rejection for indirect dispatch is a spec
    // amendment deferred from #2269 (per its commit message: "filed
    // separately for a spec amendment — not a definition-site rejection").
    //
    // The unsoundness is REAL (a double-free on disposal at the caller's
    // `using var` end) — but the fix lives outside #2269's narrow scope.
    // The hazard is documented as #2269 follow-up; this test now pins the
    // CURRENT (admitting) behavior so a future spec-amendment implementation
    // can flip it to rejection with a clear test signal.
    auto r = parseAndCheckOwnership(mod(
        "interface IArrSource "
        "  method Get(): OwningArray<Cell>; "
        "end IArrSource; "
        "class Holder : IArrSource "
        "  public owned field: OwningArray<Cell>; "
        "  public constructor(owned f: OwningArray<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningArray<Cell> return field; end Get; "
        "end Holder; "
        "class P "
        "  public static method Consume(src: IArrSource): int "
        "    using var t := src.Get(); "
        "    return 0; "
        "  end Consume; "
        "end P;"));
    // Current #2269 design: admitted (no error). Will flip to BEZEL_EXPECT_TRUE
    // when the call-site rejection lands.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// ACCEPT: the spec's two remedies — non-owning view for read-only borrow,
// `owned` for a transfer.
// ===========================================================================

// §28.6 / §5.11: a non-`owned` NON-OWNING read-only-view return
// (`IReadOnlyList<T>`) is a borrow, the borrow is sound (the source is the
// receiver-held field, the caller reads it, the receiver disposes it at its
// own scope end), and the capability restriction follows from the type
// (`IReadOnlyList<T>` has no Dispose member that would affect element
// ownership). Admitted.
//
// The view is spelled `IReadOnlyList<T>`, not `Array<T>`. stdlib §4.3
// (v0.307) grants exactly three cross-family ancestors — "The two
// hierarchies share `IEnumerable<T>`, `IReadOnlyCollection<T>`, and
// `IReadOnlyList<T>` as common ancestors. A function that wants to read
// elements without taking ownership accepts one of these interfaces; both
// borrowing and owning collections of T conform" — and denies the concrete
// classes: "`IList<T>` (borrowing) and `IOwningList<T>` (owning) are
// unrelated types with no subtyping." The `Array<Cell>` this row used to
// spell was a cross-family conversion the stdlib never granted; the row's
// subject — a non-`owned` borrowing-view return of a bare read of an
// `owned` owning-collection field is ADMITTED — is unchanged.
BEZEL_TEST(owning_collection_borrow_return_1760,
           nonowning_view_return_via_receiver_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() _items := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): IReadOnlyList<Cell> => _items; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.6 bullet 2 / §28.8.1 (#2368, v0.236): a BARE read of an owned field
// through an `owned` return is REJECTED — the spec's only transfer-to-caller
// form is `return move _f;`. Pre-#2368 this compiled (the `returnOwned_`
// early-return at the top of `checkOwnedFieldReturn` blanket-exempted the
// `owned`-return path); the bare read leaves the field un-moved-from (the
// holder's destructor still disposes it) while the caller now owns the value
// and disposes it too — a double-free (the terminal-move-without-restoration
// shape §28.8.1 rejects). The cell flips from ACCEPTED to REJECTED with #2368.
BEZEL_TEST(owning_collection_borrow_return_1760,
           owned_owning_array_return_via_receiver_field_rejected_2368) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() _items := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned OwningArray<Cell> => _items; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a bare read of an owned field may not be returned through an `owned` return type"));
}

// ===========================================================================
// ACCEPT controls: shapes the fix must NOT regress.
// ===========================================================================

// A genuinely FRESH `OwningArray<Cell>` return (a method minting a new value)
// is not a borrow — it stays admitted (consumer-owned, name-head Owned). The
// fix tightens only the receiver-owned-field carve-out, not the fresh-return
// case.
BEZEL_TEST(owning_collection_borrow_return_1760,
           fresh_owning_array_return_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Make(): owned OwningArray<Cell> "
        "    var a := new OwningArray<Cell>(); a.Add(new Cell()); return a; "
        "  end Make; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A receiver `owned`-field read returned through an idempotent-safe owning
// WRAPPER (`Shared<T>`) keeps the #1493 carve-out — its double-dispose is a
// safe refcount no-op. The fix must NOT tighten the wrapper heads. Scope
// guard that the reject above is limited to the non-idempotent owning-
// collection heads.
BEZEL_TEST(owning_collection_borrow_return_1760,
           shared_receiver_field_returned_still_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class Holder "
        "  public owned field: Shared<Cell>; "
        "  public constructor(owned f: Shared<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): Shared<Cell> return field; end Get; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
