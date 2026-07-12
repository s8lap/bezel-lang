// Codegen/runtime — issue #869: OwningLinkedList<T>.Dispose() must dispose its
// elements in REVERSE-INSERTION order (§28.11). The runtime walked the list
// tail→head, which equals reverse-insertion only when every element was
// AddLast'd; with AddFirst (or AddBefore) the head/tail position no longer
// tracks insertion order, so elements were disposed in FORWARD-insertion order.
//
// THE FIX: each node carries a monotonic insertion sequence (bz_llist_node_t.seq,
// stamped in llist_node_alloc); bz_llist_free disposes by DESCENDING sequence.
//
// Oracle: each Cell records its id into a static accumulator on disposal, so the
// final accumulator value encodes the disposal order. AddFirst(1),AddFirst(2)
// must dispose 2 then 1 → 2*10+1 = 21 (the bug produced 12). The 3-element cases
// use a base-4 fold (id < 4) to keep the exit code ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell "
    "  public static order: int := 0; "
    "  public id: int; "
    "  public constructor(i: int) id:=i; end constructor; "
    "  destructor Cell.order := Cell.order * %BASE% + id; end destructor; "
    "end Cell; ";
std::string cell(int base) {
    std::string s = kCell;
    auto pos = s.find("%BASE%");
    s.replace(pos, 6, std::to_string(base));
    return s;
}
}  // namespace

// AddFirst: insertion 1,2 → reverse-insertion disposal 2,1 → 21 (bug: 12).
BEZEL_TEST(owned_llist_dispose_order_869, add_first_reverse_insertion) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + cell(10) +
        "class C public static method Main(): int "
        "  do using var ll := new OwningLinkedList<Cell>(); "
        "     ll.AddFirst(new Cell(1)); ll.AddFirst(new Cell(2)); end; "
        "  return Cell.order; "  // 21
        "end Main; end C;"), 21);
}

// AddLast control: insertion 1,2,3 → reverse 3,2,1 → ((3)*4+2)*4+1 = 57.
BEZEL_TEST(owned_llist_dispose_order_869, add_last_still_reverse_insertion) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + cell(4) +
        "class C public static method Main(): int "
        "  do using var ll := new OwningLinkedList<Cell>(); "
        "     ll.AddLast(new Cell(1)); ll.AddLast(new Cell(2)); ll.AddLast(new Cell(3)); end; "
        "  return Cell.order; "  // 57
        "end Main; end C;"), 57);
}

// Mixed AddLast/AddFirst/AddLast: insertion seq 1,2,3 → reverse 3,2,1 → 57,
// even though positionally the list is [2, 1, 3].
BEZEL_TEST(owned_llist_dispose_order_869, mixed_add_first_last_by_insertion) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + cell(4) +
        "class C public static method Main(): int "
        "  do using var ll := new OwningLinkedList<Cell>(); "
        "     ll.AddLast(new Cell(1)); ll.AddFirst(new Cell(2)); ll.AddLast(new Cell(3)); end; "
        "  return Cell.order; "  // 57
        "end Main; end C;"), 57);
}
