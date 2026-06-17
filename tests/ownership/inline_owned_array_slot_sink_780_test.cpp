// Spec-derived tests for the owning-inline-array-slot owned-sink hole
// (issue #780, an R42 remainder item tracked by the closed #747).
//
// §28.11 (element slot policy), owned-slot column:
//   "store (Add, Insert, indexer-set) | takes `owned`; indexer-set disposes the
//    previous value first (skipped for a moved-from slot)"
//   "indexer-get / default iteration | borrow out (source: the collection)"
// The owning inline arrays `[N owned]T?` and `[N owned]T` are owning collections
// (§28.11 / §15.3): an indexer-set into a slot "takes owned." A BORROW stored
// into such a slot would be disposed by the array (its Dispose cascades through
// every owned element) AND by the borrow's real owner — a double-free, exactly
// the hazard §28.11 / §28.3 / §28.8 forbid for an `owned` field.
//
// The under-enforcement this file pins down (and #780 fixes): `a[i] := <rhs>`
// for an owning inline array routed through NO owned-sink check — a borrowed
// parameter, a bare borrowed-field read, or a scope-bound `using var` flowed
// into the owned slot with no diagnostic. The slot is now treated as an owned
// sink with the SAME discipline as an `owned` field (handleOwnedFieldAssignment
// with the owning-array-slot wording): a borrow RHS is rejected, and a bare
// owned-LOCAL read is recorded as a move INTO the slot.
//
// NEGATIVES below must now be REJECTED; POSITIVES must stay (or, for the
// owned-local transfer, BECOME) ACCEPTED — the bare owned-local read was
// previously false-rejected as "dropped without disposal" because the slot
// assignment was not recognized as a transfer.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// `class Cell` is a disposable class; `body` is the Main method body.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; class Cell public n: int := 0; end Cell; "
        "class C public static method Main(): int ") + body +
        " end Main; end C;";
}

// True iff some diagnostic is the #780 owning-array-slot borrow-store error:
// it cites §28.11 and names the owning array slot.
bool rejectsBorrowIntoSlot(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.11") == std::string::npos) continue;
        if (d.message.find("owning array slot") == std::string::npos) continue;
        return true;
    }
    return false;
}

} // namespace

// =============================================================================
// NEGATIVE — a borrow stored into an owning inline-array slot is rejected.
// =============================================================================

// §28.11 / §28.3: a borrowed parameter `c` is Borrowed (source: caller); storing
// it into an owned slot is the double-free hazard. REJECTED with the §28.11
// owning-array-slot message.
BEZEL_TEST(inline_owned_array_slot_sink_780, borrowed_param_into_slot_rejected_28_11) {
    auto r = parseAndCheckOwnership(
        "module M; class Cell public n: int := 0; end Cell; "
        "class C "
        "  public static method Stash(c: Cell): () "
        "    var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "    a[0] := c; "
        "  end Stash; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsBorrowIntoSlot(r));
}

// §28.3: a `using var` is scope-bound and cannot be moved out of; storing it
// into an owned slot (a move-on-read into the slot) is rejected. This is the
// original #780 repro. REJECTED (the move-out-of-`using var` diagnostic).
BEZEL_TEST(inline_owned_array_slot_sink_780, using_var_into_slot_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "do var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "   using var b := new Cell(); "
        "   a[0] := b; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.11 / §28.3: a non-nullable owning slot `[N owned]Cell` is equally an owned
// sink. A borrowed parameter into it is REJECTED.
BEZEL_TEST(inline_owned_array_slot_sink_780, borrowed_param_into_nonnull_slot_rejected_28_11) {
    auto r = parseAndCheckOwnership(
        "module M; class Cell public n: int := 0; end Cell; "
        "class C "
        "  public static method Stash(c: Cell): () "
        "    var a: [2 owned]Cell := [new Cell(), new Cell()]; "
        "    a[0] := c; "
        "  end Stash; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsBorrowIntoSlot(r));
}

// =============================================================================
// POSITIVE — a legal owned value flowing into a slot is accepted.
// =============================================================================

// §28.3: a bare owned-LOCAL read is a move-on-read INTO the owned slot — a legal
// transfer that leaves the local moved-from. Previously FALSE-REJECTED as
// "owned value … dropped without disposal" because the slot write was not seen
// as a transfer. Now ACCEPTED.
BEZEL_TEST(inline_owned_array_slot_sink_780, owned_local_transfer_into_slot_accepted_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "do var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "   var c := new Cell(); "
        "   a[0] := c; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.11: a fresh `new Cell()` is an owned producer — the canonical owned value
// for an owned slot. ACCEPTED.
BEZEL_TEST(inline_owned_array_slot_sink_780, new_into_slot_accepted_28_11) {
    auto r = parseAndCheckOwnership(wrap(
        "do var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "   a[0] := new Cell(); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.11: assigning `null` to a nullable owning slot is legal (it clears the
// slot); it is not a borrow store. ACCEPTED.
BEZEL_TEST(inline_owned_array_slot_sink_780, null_into_nullable_slot_accepted_28_11) {
    auto r = parseAndCheckOwnership(wrap(
        "do var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "   a[0] := null; "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
