// Spec-derived regression tests for #3295 — "§28.6's method-return exclusion:
// compiler still implements the retired four-`IOwning*` formulation, not
// v0.258's eight heads" (the STORED-VIEW portion).
//
// §28.6 (v0.258, Bezel_Language_Spec.md ~L6896): "an unmarked mint-announcing
// method return type is container-ownership-transferring (§28.3 Producers), so
// a stored frame — a stored task, a stored collection view — is re-exposed
// through a property, field, or collection slot ... — never through an
// unmarked mint-announcing method return."
//
// The §28.6 receiver-owned-field carve-out ("a bare read of an owned field ...
// may be returned through a non-`owned` return type as a borrow of the field")
// is EXPLICITLY excluded for mint-announcing-typed return types — the four
// owning iterable heads already close it (the v0.221 #2369 / #1660 deferred-
// activation carve-out); the four NON-OWNING iterable heads were the v0.221
// retirement hole and v0.258 closes them too (#3295's defect).
//
// §28.6 distinguishes the stored-frame case ("a stored collection view ... is
// re-exposed through a property, field, or collection slot") from other borrow
// shapes (a borrowed parameter passthrough, a local holding a fresh frame, a
// bare `this` from a `GetEnumerator()` body per axiom A15) — the latter are
// NOT stored frames and remain admitted; the wrapper holds a borrow of the
// call-bound value, the non-owning wrapper's `Dispose` is a no-op, and the
// caller's scope bounds the wrapper's lifetime.
//
// #3295's narrow fix: a bare read of an `owned` receiver field returned
// through an unmarked `IEnumerable<T>` / `IEnumerator<T>` /
// `IAsyncEnumerable<T>` / `IAsyncEnumerator<T>` return type is rejected. All
// other borrow shapes through the same heads remain admitted (the existing
// v0.221 behavior, retained).
//
// The four OWNING iterable heads (`IOwningEnumerable<T>`, `IOwningEnumerator<T>`,
// `IOwningAsyncEnumerable<T>`, `IOwningAsyncEnumerator<T>`) are covered by
// `owning_iterable_head_borrow_return_2369_test.cpp` (#2369 closed them under
// v0.221 via the deferredActivationOwningHead gate); #3295's contribution is
// the four NON-OWNING heads.
//
// All shapes below were hand-verified against `build/bezelc -c` on HEAD
// `5ec3439` (the four non-owning iterable heads' stored-view shapes compiled
// clean — the hole #3295 names) and with the #3295 fix (rejected at the def
// site — no compiling program can then contain the borrow-returning method).

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
// REJECT: a receiver `owned`-field bare read returned through an UNMARKED
// non-owning mint-announcing iterable-interface head. §28.6 bullet 1's
// receiver-owned-field carve-out is explicitly EXCLUDED for mint-announcing
// return types — the stored-view exclusion, v0.258, #3295.
// ===========================================================================

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           ienumerable_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningArray<Cell>; "
        "  public constructor(owned f: OwningArray<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Items(): IEnumerable<Cell> return field; end Items; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a stored view is re-exposed through the mint-announcing"));
}

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           ienumerator_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: IEnumerator<Cell>; "
        "  public constructor(owned f: IEnumerator<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): IEnumerator<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a stored view is re-exposed through the mint-announcing"));
}

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           iasync_enumerable_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: IAsyncEnumerable<Cell>; "
        "  public constructor(owned f: IAsyncEnumerable<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Items(): IAsyncEnumerable<Cell> "
        "    return field; "
        "  end Items; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a stored view is re-exposed through the mint-announcing"));
}

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           iasync_enumerator_receiver_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: IAsyncEnumerator<Cell>; "
        "  public constructor(owned f: IAsyncEnumerator<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): IAsyncEnumerator<Cell> "
        "    return field; "
        "  end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a stored view is re-exposed through the mint-announcing"));
}

// ===========================================================================
// REJECT: an `owned`-field bare read returned through an UNMARKED non-owning
// iterable head EVEN when the field is typed at the non-`owned` interface —
// the §28.6 stored-view exclusion applies to the field's READ regardless of
// the field's declared type, because the wrapper that the return constructs
// (the OwningArray<T>→IEnumerable<T> adapter, the IEnumerable<T>→
// IEnumerable<T> identity wrap, etc.) holds the storage from the receiver
// and the receiver's destructor invalidates it. Pin the broader shape — the
// non-`owned`-interface field is just as unsound as the concrete-collection
// field.
// ===========================================================================

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           ienumerable_interface_typed_field_returned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: IEnumerable<Cell>; "
        "  public constructor(owned f: IEnumerable<Cell>) field := f; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Get(): IEnumerable<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a stored view is re-exposed through the mint-announcing"));
}

// ===========================================================================
// EXEMPT controls: the EXPLICIT `owned` return type makes the transfer
// explicit, so THIS FILE's #3295 rule does not fire (the early-return on
// `returnOwned_` above short-circuits before the stored-view check).
//
// UPDATED for issue #3993: `IEnumerator<Cell>` and `IAsyncEnumerator<Cell>`
// no longer belong in this "exempt" group — they extend `IDisposable` /
// `IAsyncDisposable` (stdlib §4.12), so a borrowed-parameter passthrough
// through an `owned`-declared return of either genuinely double-frees
// (measured via ASan on a concrete `IEnumerator<T>` conformer: SEGV in the
// interface's itable dispatch on the second, already-freed disposal — the
// same crash signature `IOwningEnumerator<T>` produces). The new
// `checkOwnedReturnDirectBorrow` (§28.3 *Borrow lifetime rules*) rejects
// them; they moved to the REJECT group below. `IEnumerable<Cell>` and
// `IAsyncEnumerable<Cell>` stay exempt from THIS check's own double-dispose
// reasoning — neither extends `IDisposable`/`IAsyncDisposable` (only the
// enumerator they mint does), so there is no `.Dispose()` reachable through
// either static type for this check to have caused a second call to.
//
// ⚠ This is NOT a claim that the exempted shape is safe end-to-end: a
// concrete `IEnumerable<T>` conformer with a real destructor, borrowed
// through this exact shape and bound with `using var`, DOES crash under
// ASan — the same wild-jump SEGV every rejected head above produces, and
// measurably reachable even for a SOUND, non-borrow return through the same
// slot (`Make(): owned IEnumerable<T> => new MyE();`) — a pre-existing
// CODEGEN defect independent of what this ownership check decides, filed as
// #4007. `checkOwnedReturnDirectBorrow` carries this exemption because its
// OWN reasoning (double-dispose via two tracked bindings) does not apply
// here, not because the shape is verified safe overall.
// ===========================================================================

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           owned_return_type_exempt_for_ienumerable) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: IEnumerable<Cell>): "
        "      owned IEnumerable<Cell> => a; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           owned_return_type_exempt_for_iasync_enumerable) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: IAsyncEnumerable<Cell>): "
        "      owned IAsyncEnumerable<Cell> => a; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// REJECT (moved here from the EXEMPT group above, issue #3993): a borrowed
// parameter returned through an `owned`-declared DISPOSABLE iterable head.
// ===========================================================================

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           owned_return_type_borrow_rejected_for_ienumerator_3993) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: IEnumerator<Cell>): "
        "      owned IEnumerator<Cell> => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           owned_return_type_borrow_rejected_for_iasync_enumerator_3993) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: IAsyncEnumerator<Cell>): "
        "      owned IAsyncEnumerator<Cell> => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// ACCEPT controls: the rule rejects ONLY the stored-view case; other borrow
// shapes through the same heads remain admitted per the v0.221-pre rule.
// ===========================================================================

// A borrowed PARAMETER passthrough through the non-owning iterable heads is
// NOT a stored-frame re-exposure (the parameter is a call-bound value, not a
// property/field/slot) and remains admitted: the wrapper holds a borrow of
// the caller's frame, the wrapper's `Dispose` is a no-op, and the caller's
// scope bounds the wrapper's lifetime to the parameter's lifetime.
BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           ienumerable_parameter_passthrough_admitted) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method AsSeq(a: Array<Cell>): "
        "      IEnumerable<Cell> => a; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           ienumerator_parameter_passthrough_admitted) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: IEnumerator<Cell>): "
        "      IEnumerator<Cell> => a; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The A15 self-returning enumerator (§29.1 / §28.3 / #3289): a class
// implementing both `IEnumerable<T>` and `IEnumerator<T>` may `return this;`
// from its own `GetEnumerator()` body. The non-reclaiming disposal is what
// makes it sound where the receiver is a borrow the caller does not own.
BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           self_returning_enumerator_return_this_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C: IEnumerable<int>, IEnumerator<int> "
        "  public method GetEnumerator(): IEnumerator<int> "
        "    return this; "
        "  end GetEnumerator; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An iterator body (`yield return ...`) supplies a fresh frame; the rule
// does not fire on a fresh producer.
BEZEL_TEST(iterable_interface_head_stored_view_exclusion_3295,
           yield_return_iterator_body_admitted) {
    auto r = parseAndCheckOwnership(mod(
        "class Iter "
        "  public method Items(): IEnumerable<Cell> "
        "    yield return new Cell(); "
        "    yield return new Cell(); "
        "  end Items; "
        "end Iter;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
