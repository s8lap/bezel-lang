// Spec-derived regression tests for #2369 — "three owning-iterable heads
// missing from kOwningTypeHeads — §28.6's return-shape check never inspects
// them (#1787 ruling under-implemented)".
//
// §28.3 (v0.221, the #1787 ruling, Bezel_Language_Spec.md ~L6471): "The
// owning heads (IOwningEnumerable<T>, IOwningEnumerator<T>,
// IOwningAsyncEnumerable<T>, IOwningAsyncEnumerator<T>) have NO unmarked
// instance or extension spelling: ... so an unmarked instance or extension
// return typed at an owning head is rejected at the declaration".
//
// §28.6 (Bezel_Language_Spec.md ~L6694): "The same method-return exclusion
// applies to a field typed at an owning iterable-interface head
// (IOwningEnumerable<T>, IOwningEnumerator<T>, IOwningAsyncEnumerable<T>,
// IOwningAsyncEnumerator<T>): an unmarked instance or extension method return
// typed at an owning head is rejected at the declaration (§28.3 ... a borrowed
// view must be non-consuming, and an owning head's static type keys the
// consuming desugaring); re-expose the stored view at the concrete collection
// type or the non-owning interface (this bullet's admission), or transfer
// with an `owned` return."
//
// #1660 closed the borrow-return rejection for `OwningTask<T>` and
// `IOwningEnumerable<T>` (the two owning deferred-activation heads then in
// `kOwningTypeHeads`), including removing the #1493 receiver-owned-field
// carve-out for those two (the `deferredActivationOwningHead` gate).
// `IOwningEnumerator<T>`, `IOwningAsyncEnumerable<T>`, and
// `IOwningAsyncEnumerator<T>` were never added to `kOwningTypeHeads`, so
// `checkBorrowReturnedThroughOwningType`'s top guard
// (`!isOwningTypeNameHead(head) && !plainTaskInSyncBody`) short-circuited
// BEFORE the wrapper branch for these three — every borrow (a parameter, a
// field of another object, an index read, AND a receiver-owned-field bare
// read) sailed past the §28.6 check that the #1787 ruling requires to reject.
// Under indirect (interface / virtual-override / imported / function-value)
// dispatch the caller cannot resolve the concrete method, producerState falls
// to the name-head Owned fallback, and the caller disposes the borrow — a
// HARD double-free of the borrow's real owner (the same hazard #1660 closed
// for `IOwningEnumerable<T>`).
//
// The fix has two parts, both exercised here:
//   1. Add the three heads to `kOwningTypeHeads` (the top guard now lets the
//      wrapper branch inspect them — rejects parameter/local/index/other-
//      object-field borrows).
//   2. Widen the `deferredActivationOwningHead` gate from
//      `head == "IOwningEnumerable"` to all four owning iterable-interface
//      heads (`isOwningIterableInterfaceHead`) — removes the receiver-owned-
//      field carve-out for the three siblings too, per §28.6 bullet 1's
//      explicit exclusion of "a field typed at an owning iterable-interface
//      head" from the bare-read admission.
//
// All shapes below were hand-verified against `build/bezelc -c` on HEAD
// (the three new heads' borrow shapes compiled clean — the hole) and with the
// fix (rejected at the def site — no compiling program can then contain the
// borrow-returning method, and no caller can double-free).

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

} // namespace

// ===========================================================================
// REJECT: a borrow (plain parameter passthrough) returned through an OWNING
// iterable-interface head. The issue's core repro — on HEAD the three new
// heads sailed past `checkBorrowReturnedThroughOwningType` entirely.
// ===========================================================================

// `IOwningEnumerable<T>` control (already in kOwningTypeHeads pre-#2369) —
// pins the whole four-head surface and proves the test infra fires.
BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           iowning_enumerable_param_passthrough_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(src: IOwningEnumerable<Cell>): "
        "      IOwningEnumerable<Cell> => src; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// `IOwningEnumerator<T>` — the first missing head. Pre-fix: admitted (the
// hole); post-fix: rejected at the def site.
BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           iowning_enumerator_param_passthrough_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(src: IOwningEnumerator<Cell>): "
        "      IOwningEnumerator<Cell> => src; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// `IOwningAsyncEnumerable<T>` — the second missing head.
BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           iowning_async_enumerable_param_passthrough_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(src: IOwningAsyncEnumerable<Cell>): "
        "      IOwningAsyncEnumerable<Cell> => src; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// `IOwningAsyncEnumerator<T>` — the third missing head.
BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           iowning_async_enumerator_param_passthrough_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(src: IOwningAsyncEnumerator<Cell>): "
        "      IOwningAsyncEnumerator<Cell> => src; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// ===========================================================================
// REJECT: a receiver `owned`-field bare read returned through an OWNING
// iterable-interface head. §28.6 bullet 1 EXPLICITLY excludes "a field typed
// at an owning iterable-interface head" from the bare-read borrow admission;
// the `deferredActivationOwningHead` gate (#1660) removes the #1493 carve-out.
// #2369 extends that gate from `IOwningEnumerable` alone to all four owning
// iterable heads — without the extension these three would admit the shape
// (the carve-out fires) and double-free under indirect dispatch.
// ===========================================================================

BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           iowning_enumerator_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: IOwningEnumerator<Cell>; "
        "  public constructor(owned f: IOwningEnumerator<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): IOwningEnumerator<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           iowning_async_enumerable_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: IOwningAsyncEnumerable<Cell>; "
        "  public constructor(owned f: IOwningAsyncEnumerable<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): IOwningAsyncEnumerable<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           iowning_async_enumerator_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: IOwningAsyncEnumerator<Cell>; "
        "  public constructor(owned f: IOwningAsyncEnumerator<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): IOwningAsyncEnumerator<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned through the owning-type"));
}

// ===========================================================================
// ACCEPT controls: shapes the fix must NOT regress.
// ===========================================================================

// UPDATED for issue #3993: these three rows originally asserted that an
// explicit `owned` return type EXEMPTS the return from THIS file's #2369
// rule ("`checkBorrowReturnedThroughOwningType` early-returns on
// `returnOwned_` and the rule does not fire") — true of #2369's OWN narrow
// scope (it only ever ran for the UNMARKED case), but nothing else verified
// that the returned expression is actually owned when `owned` IS declared:
// `a` is a plain borrowed parameter, and each of these four heads extends
// `IDisposable` (`IOwningEnumerator<T>`, `IOwningAsyncEnumerable<T>`,
// `IOwningAsyncEnumerator<T>` — stdlib §4.13) — so labeling the borrow
// "owned" hands the caller a second owner of an object its real owner also
// disposes. Measured via ASan on a concrete `IOwningEnumerator<T>`
// conformer with a real destructor, borrowed through this exact `Id` shape
// and returned `owned`: SEGV in the interface's itable dispatch on the
// second (already-freed) disposal. The new `checkOwnedReturnDirectBorrow`
// (§28.3 *Borrow lifetime rules*) rejects all three — NOT exempted by
// `owned`, exactly like the unmarked case `owning_iterable_head_borrow_
// return_2369`'s REJECT rows above already cover. (`IOwningEnumerable<T>`,
// the fourth owning head, is covered by
// `borrow_return_owning_type_1007_test.cpp`'s analogous update — this file
// never had an "exempt" row for it.)
BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           owned_return_type_borrow_rejected_for_iowning_enumerator_3993) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: IOwningEnumerator<Cell>): "
        "      owned IOwningEnumerator<Cell> => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           owned_return_type_borrow_rejected_for_iowning_async_enumerable_3993) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: IOwningAsyncEnumerable<Cell>): "
        "      owned IOwningAsyncEnumerable<Cell> => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           owned_return_type_borrow_rejected_for_iowning_async_enumerator_3993) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: IOwningAsyncEnumerator<Cell>): "
        "      owned IOwningAsyncEnumerator<Cell> => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The genuine owned producer through the SAME owned-declared return type is
// NOT over-rejected — a fresh producing call result is not a borrow.
// `IOwningEnumerator<Cell>` has no user-declarable `new` form reachable
// without an owning collection; use the stdlib factory chain instead —
// `OwningArray<Cell>.Drain()` mints a fresh `IOwningEnumerable<Cell>` view
// (stdlib §4.5), whose `.GetOwningEnumerator()` mints the fresh enumerator
// (stdlib §4.13) — to keep the control minimal and self-contained.
BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           owned_return_type_fresh_producer_for_iowning_enumerator_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Make(a: OwningArray<Cell>): "
        "      owned IOwningEnumerator<Cell> => a.Drain().GetOwningEnumerator(); "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// FALSE-POSITIVE GUARD: the NON-owning iterable heads (`IEnumerator<T>`,
// `IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`) are NOT consumer-disposed — a
// borrow aliased through them is harmless because no caller ever disposes the
// result (§28.3 v0.184). They must still ADMIT a parameter passthrough after
// the #2369 widening: the rejection targets ONLY the four OWNING iterable
// heads, not the iterator-convention family at large.
BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           nonowning_ienumerator_param_passthrough_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(src: IEnumerator<Cell>): "
        "      IEnumerator<Cell> => src; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           nonowning_iasync_enumerable_param_passthrough_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(src: IAsyncEnumerable<Cell>): "
        "      IAsyncEnumerable<Cell> => src; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(owning_iterable_head_borrow_return_2369,
           nonowning_iasync_enumerator_param_passthrough_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(src: IAsyncEnumerator<Cell>): "
        "      IAsyncEnumerator<Cell> => src; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
