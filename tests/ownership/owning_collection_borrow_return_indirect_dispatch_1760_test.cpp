// Spec-derived tests for #1760 "receiver-owned-field borrow return through an
// owning-COLLECTION (OwningArray/…) double-frees under indirect dispatch"
// (§28.6 Part C / §5.11, v0.184) — the wider owning-collection analogue of
// #1660's deferred-activation fix.
//
// #1660 closed the unsound `return this._field;` carve-out for the OWNING
// DEFERRED-ACTIVATION heads (`OwningTask<T>`, `IOwningEnumerable<T>`) on the
// ground that under interface / virtual-override / imported / function-value
// dispatch the call site cannot resolve the concrete method, producerState
// falls to the name-head Owned fallback, and the caller disposes the borrow —
// a HARD double-free of the field's real owner. The carve-out is sound only
// for a DIRECT call (where producerState resolves the concrete all-borrow
// method and downgrades the result to Borrowed), and the deferred-activation
// heads are NOT Dispose-idempotent (unlike Shared/Weak/ChannelSender/
// CancellationTokenSource, whose double-dispose is a safe refcount no-op).
//
// #1760 extends the same fix to the OWNING COLLECTIONS (OwningArray,
// OwningDictionary, OwningSet, OwningStack, OwningQueue, OwningLinkedList).
// The pre-existing receiver-owned-field carve-out for these heads (the
// "§28.6 / §5.11 (#1008 Part C) carve-out" in `checkBorrowReturnedThroughOwning
// Type`) was unsound for the same reason — name-head Owned fallback on
// non-idempotent types. The compiler therefore REJECTS every receiver-owned-
// field borrow return through an owning collection at the definition site, so
// no compiling program can contain the borrow-returning method and no caller
// can double-free. The remedy is the spec's own: return the borrow through the
// non-owning collection type (`Array<T>`/etc. instead of `OwningArray<T>`/etc.)
// for read-only access, or declare `owned` for a transfer.
//
// All shapes below were hand-verified against `build/bezelc -c` (and the
// reject shapes additionally against native AOT+ASan) before being folded in.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

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
// REJECT: a borrow returned through an OWNING COLLECTION head — the
// receiver-owned-field carve-out (the #1008 Part C exception) is removed.
// ===========================================================================

// `OwningArray<T>`: a bare read of an `owned` field of the receiver returned
// through a non-`owned` `OwningArray<Cell>` — the #1008 Part C carve-out
// shape, rejected at the definition site per #1760. The concrete method is
// checked regardless of how it is called, so no compiling program can contain
// it and no caller can double-free.
BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_array_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningArray<Cell>; "
        "  public constructor(owned f: OwningArray<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningArray<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The other owning collection heads — same shape, all rejected at the def
// site because all six entries in `kOwningCollectionHeads` are non-idempotent
// (Dispose is a hard free; verified for each).
BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_dictionary_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningDictionary<int, Cell>; "
        "  public constructor(owned f: OwningDictionary<int, Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningDictionary<int, Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_set_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningSet<Cell>; "
        "  public constructor(owned f: OwningSet<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningSet<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_stack_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningStack<Cell>; "
        "  public constructor(owned f: OwningStack<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningStack<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_queue_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningQueue<Cell>; "
        "  public constructor(owned f: OwningQueue<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningQueue<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_linked_list_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningLinkedList<Cell>; "
        "  public constructor(owned f: OwningLinkedList<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningLinkedList<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A bare identifier (`_items`, implicit `this._items`) — the #1008 Part C
// carve-out admitted this; the fix rejects it the same way.
BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_array_bare_identifier_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() _items := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningArray<Cell> => _items; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An explicit `this._items` — the #1008 Part C carve-out admitted this; the
// fix rejects it the same way.
BEZEL_TEST(owning_collection_borrow_return_1760,
           owning_array_explicit_this_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() _items := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningArray<Cell> => this._items; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
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
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// ACCEPT: the spec's two remedies — non-owning view for read-only borrow,
// `owned` for a transfer.
// ===========================================================================

// §28.6 / §5.11: a non-`owned` NON-OWNING collection return (`Array<T>`) is
// a borrow, the borrow is sound (the source is the receiver-held field, the
// caller reads it, the receiver disposes it at its own scope end), and the
// capability restriction follows from the type (Array<T> has no Dispose
// method that would affect element ownership). Admitted.
BEZEL_TEST(owning_collection_borrow_return_1760,
           nonowning_array_return_via_receiver_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() _items := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): Array<Cell> => _items; "
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
