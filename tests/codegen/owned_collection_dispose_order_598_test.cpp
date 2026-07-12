// Codegen — ordered owning collections dispose elements in reverse-insertion
// order (issue #598, §28.11).
//
// §28.11 (spec line 5547): "The element-disposal order is
// implementation-defined for unordered collections (OwningDictionary, OwningSet),
// and is reverse-insertion-order for ordered collections (OwningArray,
// OwningLinkedList, OwningStack, OwningQueue)."
//
// The reproducer from #598: Add Item(1), Item(2), Item(3) to an owning
// collection. Each item's destructor encodes its id in a static log via
//   _log := _log * 10 + id
// Expected (reverse-insertion = last-in-first-out): 3→2→1 → _log == 321.
// Before the fix, all four ordered collections disposed forward → _log == 123.
//
// OwningSet is unordered (order is impl-defined); it is NOT tested here.
//
// AOT MIGRATION (#1769 / #1763): this file runs under the AOT driver (runAot).
// The dispose-order observable is `_log == 321`, which exceeds the 8-bit process
// exit channel (WEXITSTATUS masks to 0..255; 321 & 0xFF == 65). Per the migration
// convention (aot_support.hpp; int_arithmetic_test.cpp), the order tests print
// `_log` with Console.WriteLine (stdlib §2.1) and assert the captured stdout,
// while the ≤255 observables (single-element = 7, count = 3) round-trip the exit
// code. AOT was verified to dispose in the SAME reverse-insertion order the JIT
// did (stdout "321"); the JIT→AOT divergence #1769 recorded for this file was
// purely the 8-bit exit channel, NOT a disposal-order difference.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// A class whose destructor appends its id to a shared static log.
// _log starts at 0; disposal of Item(id) turns _log into _log*10 + id.
// Three items with ids 1,2,3 in reverse order → _log == 321.
std::string makeLoggingItems(const std::string& mainBody) {
    return
        "module M; "
        "class Item "
        "  public static _log: int := 0; "
        "  private _id: int := 0; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  destructor _log := _log * 10 + _id; end destructor; "
        "end Item; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;";
}

// ≤255 observable (single-element = 7): the Main body returns _log so the exit
// code is the observable result.
int runItems(const std::string& mainBody) {
    return run(makeLoggingItems(mainBody));
}

// Order observable (_log == 321) exceeds the 8-bit exit channel: `doBlock` runs
// the collection to scope-end disposal, then _log is printed to stdout. Returns
// the captured stdout (e.g. "321\n").
std::string runItemsOut(const std::string& doBlock) {
    auto r = runAot(makeLoggingItems(
        doBlock + " Console.WriteLine(Item._log); return 0;"));
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ---------------------------------------------------------------------------
// OwningArray<T> — §28.11
// ---------------------------------------------------------------------------

// §28.11: OwningArray<T>.Dispose() (via `using var` scope-end) disposes
// elements in reverse-insertion order. Add(Item(1)); Add(Item(2)); Add(Item(3))
// → dispose 3 first, then 2, then 1 → _log == 321.
// Before fix: forward order → _log == 123.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_array_disposes_reverse_insertion_order) {
    BEZEL_EXPECT_EQ(runItemsOut(
        "do "
        "  using var a := new OwningArray<Item>(); "
        "  a.Add(move (new Item(1))); "
        "  a.Add(move (new Item(2))); "
        "  a.Add(move (new Item(3))); "
        "end;"), "321\n");
}

// §28.11: single-element OwningArray disposes the single element exactly once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_array_single_element_disposed_once) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var a := new OwningArray<Item>(); "
        "  a.Add(move (new Item(7))); "
        "end; "
        "return Item._log;"), 7);
}

// §28.11: regression — all elements are disposed exactly once (no leak,
// no double-dispose). Count via static counter incremented in destructor.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_array_all_elements_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  do "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(move (new R())); "
        "    a.Add(move (new R())); "
        "    a.Add(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// OwningLinkedList<T> — §28.11
// ---------------------------------------------------------------------------

// §28.11: OwningLinkedList<T>.Dispose() disposes elements in reverse-insertion
// order. AddLast(Item(1)); AddLast(Item(2)); AddLast(Item(3)) → dispose 3
// first, then 2, then 1 → _log == 321.
// Before fix: forward (head-to-tail) order → _log == 123.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_linked_list_disposes_reverse_insertion_order) {
    BEZEL_EXPECT_EQ(runItemsOut(
        "do "
        "  using var ll := new OwningLinkedList<Item>(); "
        "  ll.AddLast(move (new Item(1))); "
        "  ll.AddLast(move (new Item(2))); "
        "  ll.AddLast(move (new Item(3))); "
        "end;"), "321\n");
}

// §28.11: single-element OwningLinkedList disposes the single element once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_linked_list_single_element_disposed_once) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var ll := new OwningLinkedList<Item>(); "
        "  ll.AddLast(move (new Item(7))); "
        "end; "
        "return Item._log;"), 7);
}

// §28.11: regression — all elements disposed exactly once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_linked_list_all_elements_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  do "
        "    using var ll := new OwningLinkedList<R>(); "
        "    ll.AddLast(move (new R())); "
        "    ll.AddLast(move (new R())); "
        "    ll.AddLast(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// OwningQueue<T> — §28.11
// ---------------------------------------------------------------------------

// §28.11: OwningQueue<T>.Dispose() disposes elements in reverse-insertion
// order. Enqueue(Item(1)); Enqueue(Item(2)); Enqueue(Item(3)) inserts 1 first
// (front) and 3 last (back). Reverse-insertion → dispose 3 first, then 2,
// then 1 → _log == 321.
// Before fix: forward order → _log == 123.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_queue_disposes_reverse_insertion_order) {
    BEZEL_EXPECT_EQ(runItemsOut(
        "do "
        "  using var q := new OwningQueue<Item>(); "
        "  q.Enqueue(move (new Item(1))); "
        "  q.Enqueue(move (new Item(2))); "
        "  q.Enqueue(move (new Item(3))); "
        "end;"), "321\n");
}

// §28.11: single-element OwningQueue disposes the single element once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_queue_single_element_disposed_once) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var q := new OwningQueue<Item>(); "
        "  q.Enqueue(move (new Item(7))); "
        "end; "
        "return Item._log;"), 7);
}

// §28.11: regression — all elements disposed exactly once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_queue_all_elements_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  do "
        "    using var q := new OwningQueue<R>(); "
        "    q.Enqueue(move (new R())); "
        "    q.Enqueue(move (new R())); "
        "    q.Enqueue(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// OwningStack<T> — §28.11
// ---------------------------------------------------------------------------

// §28.11: OwningStack<T>.Dispose() disposes elements in reverse-insertion
// order. Push(Item(1)); Push(Item(2)); Push(Item(3)) inserts 1 first (bottom)
// and 3 last (top). Reverse-insertion means dispose 3 (top/last-in) first,
// then 2, then 1 → _log == 321. Note: Pop order is 3→2→1 (LIFO), which
// coincides with reverse-insertion order; the spec mandates this for Dispose.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_stack_disposes_reverse_insertion_order) {
    BEZEL_EXPECT_EQ(runItemsOut(
        "do "
        "  using var s := new OwningStack<Item>(); "
        "  s.Push(move (new Item(1))); "
        "  s.Push(move (new Item(2))); "
        "  s.Push(move (new Item(3))); "
        "end;"), "321\n");
}

// §28.11: single-element OwningStack disposes the single element once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_stack_single_element_disposed_once) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var s := new OwningStack<Item>(); "
        "  s.Push(move (new Item(7))); "
        "end; "
        "return Item._log;"), 7);
}

// §28.11: regression — all elements disposed exactly once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_stack_all_elements_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  do "
        "    using var s := new OwningStack<R>(); "
        "    s.Push(move (new R())); "
        "    s.Push(move (new R())); "
        "    s.Push(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// OwningArray field (owned field in a class) — §28.11 + §28.5
// ---------------------------------------------------------------------------

// §28.11 + §28.5: an `owned OwningArray<T>` field's elements are disposed
// when the owner is disposed. Disposal of the owner triggers the field's
// backing to be freed, cascading reverse-insertion-order element disposal.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_array_field_disposes_reverse_insertion_order) {
    // Order observable (321) exceeds the 8-bit exit channel → stdout.
    auto r = runAot(
        "module M; "
        "class Item "
        "  public static _log: int := 0; "
        "  private _id: int := 0; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  destructor _log := _log * 10 + _id; end destructor; "
        "end Item; "
        "class Pool "
        "  private owned _items: OwningArray<Item>; "
        "  public constructor() "
        "    _items := new OwningArray<Item>(); "
        "    _items.Add(move (new Item(1))); "
        "    _items.Add(move (new Item(2))); "
        "    _items.Add(move (new Item(3))); "
        "  end constructor; "
        "end Pool; "
        "class C public static method Main(): int "
        "  do "
        "    using var p := new Pool(); "
        "  end; "
        "  Console.WriteLine(Item._log); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "321\n");
}
