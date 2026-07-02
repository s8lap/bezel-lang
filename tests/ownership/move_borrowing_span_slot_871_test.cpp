// Spec-derived tests for issue #871 — reject `move arrSpan[i]` through a
// BORROWING-span indexer slot.
//
// Spec basis. §28.11 ("`move` on a collection slot", line 5629) is explicit:
// "The same applies to ArraySpan<T> and ReadOnlyArraySpan<T> (stdlib §4.15),
// which view into a borrowing Array<T> source: `move arrSpan[i]` is rejected at
// compile time." The stdlib §4.15 ownership table likewise lists ArraySpan<T> as
// not admitting `move` for any T — the span's slots hold BORROWS of the backing
// Array<T>'s elements, so there is no ownership to transfer.
//
// Before this fix sema did not gate `move`-from-indexer by the receiver's span
// type, so `move sp[0]` on an `ArraySpan<T>` compiled and then SEGFAULTED at
// runtime (it tried to transfer ownership out of a borrow-backed slot). Per
// §28.11 / §4.15 it must be a compile error.
//
// SCOPE. The diagnostic fires when the `move`d index expression's receiver is a
// BORROWING span — `ArraySpan<T>` / `ReadOnlyArraySpan<T>` (views over a
// borrowing `Array<T>`) or a read-only span (`ReadOnlySpan<T>`,
// `IReadOnlySpan<T>`, `ReadOnlyStringSpan`, `ReadOnlyAsciiByteSpan`). The OWNING
// `Span<T>` (over an `OwningBuffer`) and the concrete owning collections
// (`move OwningArray[i]`) stay accepted, and a plain span indexer-get (no `move`)
// stays accepted.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::OwnershipResult;

namespace {

// True iff some diagnostic is the #871 reject-move-through-borrowing-span error:
// it cites §28.11 / §4.15 and names the span type.
bool rejectsMoveThroughSpan(const OwnershipResult& r, std::string_view spanName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.11") == std::string::npos &&
            d.message.find("§4.15") == std::string::npos)
            continue;
        if (d.message.find("move") == std::string::npos) continue;
        if (d.message.find(std::string(spanName)) == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — `move span[i]` through a borrowing span is rejected.
// =============================================================================

// §28.11 / §4.15: `sp` has static type `ArraySpan<Cell>` from `Array<Cell>.AsSpan()`
// — a borrowing view whose slots are borrows. `move sp[0]` is a compile error.
// This is the exact #871 repro that compiled and SEGFAULTED before the fix.
BEZEL_TEST(move_borrowing_span_slot_871, move_through_array_span_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end; destructor end; end Cell; "
        "class C public static method Main(): int "
        "  using var arr := new Array<Cell>(); "
        "  using var c1 := new Cell(); "
        "  arr.Add(c1); "
        "  var sp := arr.AsSpan(); "
        "  using var taken := move sp[0]; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughSpan(r, "ArraySpan"));
}

// §28.11 / §4.15: `ReadOnlyArraySpan<Cell>` is read-only and borrowing — `move`
// through it is rejected for the same reason.
BEZEL_TEST(move_borrowing_span_slot_871, move_through_readonly_array_span_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end; destructor end; end Cell; "
        "class C public static method Main(): int "
        "  using var arr := new Array<Cell>(); "
        "  using var c1 := new Cell(); "
        "  arr.Add(c1); "
        "  var sp := arr.AsReadOnlySpan(); "
        "  using var taken := move sp[0]; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMoveThroughSpan(r, "ReadOnlyArraySpan"));
}

// =============================================================================
// POSITIVE — the legal forms stay accepted (no over-rejection).
// =============================================================================

// §28.11: `move owner[0]` on a CONCRETE `OwningArray<Cell>` is an enumerated
// admitting receiver — still accepted. The span gate must not touch it.
BEZEL_TEST(move_borrowing_span_slot_871, move_through_owned_array_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end; destructor end; end Cell; "
        "class C public static method Main(): int "
        "  using var owner := new OwningArray<Cell>(); "
        "  owner.Add(move (new Cell())); "
        "  using var x := move owner[0]; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.11 / §4.15: a plain indexer-get on an `ArraySpan<T>` (no `move`) reads the
// borrow and is unaffected by this fix — still accepted.
BEZEL_TEST(move_borrowing_span_slot_871, plain_array_span_get_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public V: int := 0; end Cell; "
        "class C public static method Main(): int "
        "  using var arr := new Array<Cell>(); "
        "  using var c1 := new Cell(); "
        "  arr.Add(c1); "
        "  var sp := arr.AsSpan(); "
        "  var v := sp[0].V; "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
