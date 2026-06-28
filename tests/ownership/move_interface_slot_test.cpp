// Spec-derived tests for issue #423 — reject `move coll[i]` through an
// INTERFACE-typed indexer slot.
//
// Spec basis. §28.8.1 ("The `move` expression") says `move coll[i]` is the
// collection-slot analogue of `move x.f`, and §28.11 ("`move` on a collection
// slot") EXPLICITLY ENUMERATES the receivers that admit it:
//
//   - owning heap collections that support indexer syntax — `OwnedArray<T>`,
//     `OwnedDictionary<K, V>`, `OwnedBuffer<T>` for class-typed T;
//   - `Span<T>` for class-typed T;
//   - owning inline arrays `[N owned]T?` and `[N owned]T`.
//
// An INTERFACE-typed receiver (`IOwnedList<T>`, `IList<T>`, `IReadOnlyList<T>`,
// or a user-declared interface that happens to expose an indexer) is NOT among
// the enumerated admitting receivers. The interface itself owns nothing — the
// owning storage lives in whatever concrete class implements it — so there is no
// statically-known slot to leave in moved-from state. Codegen's
// `lowerMoveFromCollectionSlot` correctly recognizes only the concrete owning
// collections; before this fix sema/ownership ACCEPTED `move xs[i]` on an
// interface-typed `xs`, the moved-out element was mishandled, and the program
// double-freed / SEGFAULTED at runtime. Per §28.8.1 / §28.11 this must be a
// compile error.
//
// SCOPE. The diagnostic fires when the `move`d index expression's receiver is an
// interface type — a user `interface` (Named TypeRef with a declaration whose
// kind is Interface) or a built-in collection interface (`IOwnedList`, `IList`,
// `IReadOnlyList`, and the other auto-imported `I…`/`Owned…` interfaces, which
// resolve to a Named TypeRef with a null declaration). The legal CONCRETE forms
// (`move OwnedArray[i]`, `move OwnedDictionary[k]`) stay accepted, and a
// BORROWING indexer-get on an interface (no `move`) stays accepted.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::OwnershipResult;

namespace {

// A disposable resource class R + a holder class C with the given Main body.
std::string wrap(const std::string& mainBody) {
    return std::string(
        "module M; "
        "class R end R; "
        "class C public static method Main() ") + mainBody +
        " end Main; end C;";
}

// True iff some diagnostic is the #423 reject-move-through-interface-slot error:
// it cites §28.8.1 / §28.11 and names the interface type.
bool rejectsMoveThroughInterface(const OwnershipResult& r,
                                 std::string_view ifaceName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.8.1") == std::string::npos &&
            d.message.find("§28.11") == std::string::npos)
            continue;
        if (d.message.find("move") == std::string::npos) continue;
        if (d.message.find(std::string(ifaceName)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

} // namespace

// =============================================================================
// NEGATIVE — `move xs[i]` through an interface-typed slot is now rejected.
// =============================================================================

// §28.8.1 / §28.11: `xs` has static type `IOwnedList<R>` (a built-in owning-list
// INTERFACE; `OwnedArray<R>: IOwnedList<R>`). The interface is not an enumerated
// admitting receiver for `move coll[i]`, so `move xs[0]` is a compile error.
// This is the exact repro from #423 that compiled and SEGFAULTED before the fix.
BEZEL_TEST(move_iface_slot_28_8_1, move_through_iownedlist_slot_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "using var owner := new OwnedArray<R>(); "
        "owner.Add(move (new R())); "
        "var xs: IOwnedList<R> := owner; "
        "using var x := move xs[0]; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughInterface(r, "IOwnedList"));
}

// §28.8.1 / §28.11: `IList<R>` (a borrowing-list interface) is likewise not an
// admitting receiver — its slots hold borrows, there is nothing to move out.
// `Array<R>: IList<R>`, so the upcast is legal but the `move` is not.
BEZEL_TEST(move_iface_slot_28_11, move_through_ilist_slot_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "using var backing := new Array<R>(); "
        "var xs: IList<R> := backing; "
        "using var x := move xs[0]; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughInterface(r, "IList"));
}

// §28.8.1 / §28.11: `IReadOnlyList<R>` is a read-only interface; `move` through
// it is rejected for the same reason — no enumerated admitting receiver.
BEZEL_TEST(move_iface_slot_28_11, move_through_ireadonlylist_slot_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "using var owner := new OwnedArray<R>(); "
        "owner.Add(move (new R())); "
        "var xs: IReadOnlyList<R> := owner; "
        "using var x := move xs[0]; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughInterface(r, "IReadOnlyList"));
}

// §28.8.1 / §28.11: a USER-declared interface that exposes an int-indexer is
// also not an admitting receiver. The interface owns no storage; `move` through
// it is rejected and the diagnostic names the interface.
BEZEL_TEST(move_iface_slot_28_8_1, move_through_user_interface_slot_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class R end R; "
        "public interface IBag "
        "  property [i: int]: R; "
        "end IBag; "
        "class C public static method Use(b: IBag) "
        "  using var x := move b[0]; "
        "end Use; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughInterface(r, "IBag"));
}

// =============================================================================
// POSITIVE — the legal concrete forms and a borrowing interface read stay clean.
// =============================================================================

// §28.11: `move owner[0]` on a CONCRETE `OwnedArray<R>` is an enumerated
// admitting receiver — still accepted, no over-rejection. The slot is restored
// at the `using var`-bound scope; the array is disposed at its own scope end.
BEZEL_TEST(move_iface_slot_28_11, move_through_concrete_owned_array_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var owner := new OwnedArray<R>(); "
        "owner.Add(move (new R())); "
        "using var x := move owner[0]; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.11 / stdlib §4.9: `move dict[key]` on a CONCRETE `OwnedDictionary<K, V>`
// is an enumerated admitting receiver — still accepted.
BEZEL_TEST(move_iface_slot_28_11, move_through_concrete_owned_dict_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var d := new OwnedDictionary<string, R>(); "
        "d[\"a\"] := move (new R()); "
        "using var x := move d[\"a\"]; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.11: a BORROWING indexer-get `xs[0]` (no `move`) on an interface-typed
// receiver still reads — the indexer-get produces a borrow and is unaffected by
// this fix. Touch a field of the borrowed element to force the read.
BEZEL_TEST(move_iface_slot_28_11, borrowing_interface_indexer_get_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class R public V: int := 0; end R; "
        "class C public static method Use(xs: IOwnedList<R>) "
        "  var v := xs[0].V; "
        "end Use; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
