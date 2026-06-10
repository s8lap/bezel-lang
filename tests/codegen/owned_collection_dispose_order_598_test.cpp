// Codegen — ordered owning collections dispose elements in reverse-insertion
// order (issue #598, §28.11).
//
// §28.11 (spec line 5547): "The element-disposal order is
// implementation-defined for unordered collections (OwnedDictionary, OwnedSet),
// and is reverse-insertion-order for ordered collections (OwnedArray,
// OwnedLinkedList, OwnedStack, OwnedQueue)."
//
// The reproducer from #598: Add Item(1), Item(2), Item(3) to an owning
// collection. Each item's destructor encodes its id in a static log via
//   _log := _log * 10 + id
// Expected (reverse-insertion = last-in-first-out): 3→2→1 → _log == 321.
// Before the fix, all four ordered collections disposed forward → _log == 123.
//
// OwnedSet is unordered (order is impl-defined); it is NOT tested here.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// A class whose destructor appends its id to a shared static log.
// _log starts at 0; disposal of Item(id) turns _log into _log*10 + id.
// Three items with ids 1,2,3 in reverse order → _log == 321.
// The Main body must return _log so the exit code is the observable result.
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

int runItems(const std::string& mainBody) {
    return run(makeLoggingItems(mainBody));
}

}  // namespace

// ---------------------------------------------------------------------------
// OwnedArray<T> — §28.11
// ---------------------------------------------------------------------------

// §28.11: OwnedArray<T>.Dispose() (via `using var` scope-end) disposes
// elements in reverse-insertion order. Add(Item(1)); Add(Item(2)); Add(Item(3))
// → dispose 3 first, then 2, then 1 → _log == 321.
// Before fix: forward order → _log == 123.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_array_disposes_reverse_insertion_order) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var a := new OwnedArray<Item>(); "
        "  a.Add(move (new Item(1))); "
        "  a.Add(move (new Item(2))); "
        "  a.Add(move (new Item(3))); "
        "end; "
        "return Item._log;"), 321);
}

// §28.11: single-element OwnedArray disposes the single element exactly once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_array_single_element_disposed_once) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var a := new OwnedArray<Item>(); "
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
        "    using var a := new OwnedArray<R>(); "
        "    a.Add(move (new R())); "
        "    a.Add(move (new R())); "
        "    a.Add(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// OwnedLinkedList<T> — §28.11
// ---------------------------------------------------------------------------

// §28.11: OwnedLinkedList<T>.Dispose() disposes elements in reverse-insertion
// order. AddLast(Item(1)); AddLast(Item(2)); AddLast(Item(3)) → dispose 3
// first, then 2, then 1 → _log == 321.
// Before fix: forward (head-to-tail) order → _log == 123.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_linked_list_disposes_reverse_insertion_order) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var ll := new OwnedLinkedList<Item>(); "
        "  ll.AddLast(move (new Item(1))); "
        "  ll.AddLast(move (new Item(2))); "
        "  ll.AddLast(move (new Item(3))); "
        "end; "
        "return Item._log;"), 321);
}

// §28.11: single-element OwnedLinkedList disposes the single element once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_linked_list_single_element_disposed_once) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var ll := new OwnedLinkedList<Item>(); "
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
        "    using var ll := new OwnedLinkedList<R>(); "
        "    ll.AddLast(move (new R())); "
        "    ll.AddLast(move (new R())); "
        "    ll.AddLast(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// OwnedQueue<T> — §28.11
// ---------------------------------------------------------------------------

// §28.11: OwnedQueue<T>.Dispose() disposes elements in reverse-insertion
// order. Enqueue(Item(1)); Enqueue(Item(2)); Enqueue(Item(3)) inserts 1 first
// (front) and 3 last (back). Reverse-insertion → dispose 3 first, then 2,
// then 1 → _log == 321.
// Before fix: forward order → _log == 123.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_queue_disposes_reverse_insertion_order) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var q := new OwnedQueue<Item>(); "
        "  q.Enqueue(move (new Item(1))); "
        "  q.Enqueue(move (new Item(2))); "
        "  q.Enqueue(move (new Item(3))); "
        "end; "
        "return Item._log;"), 321);
}

// §28.11: single-element OwnedQueue disposes the single element once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_queue_single_element_disposed_once) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var q := new OwnedQueue<Item>(); "
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
        "    using var q := new OwnedQueue<R>(); "
        "    q.Enqueue(move (new R())); "
        "    q.Enqueue(move (new R())); "
        "    q.Enqueue(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// OwnedStack<T> — §28.11
// ---------------------------------------------------------------------------

// §28.11: OwnedStack<T>.Dispose() disposes elements in reverse-insertion
// order. Push(Item(1)); Push(Item(2)); Push(Item(3)) inserts 1 first (bottom)
// and 3 last (top). Reverse-insertion means dispose 3 (top/last-in) first,
// then 2, then 1 → _log == 321. Note: Pop order is 3→2→1 (LIFO), which
// coincides with reverse-insertion order; the spec mandates this for Dispose.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_stack_disposes_reverse_insertion_order) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var s := new OwnedStack<Item>(); "
        "  s.Push(move (new Item(1))); "
        "  s.Push(move (new Item(2))); "
        "  s.Push(move (new Item(3))); "
        "end; "
        "return Item._log;"), 321);
}

// §28.11: single-element OwnedStack disposes the single element once.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_stack_single_element_disposed_once) {
    BEZEL_EXPECT_EQ(runItems(
        "do "
        "  using var s := new OwnedStack<Item>(); "
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
        "    using var s := new OwnedStack<R>(); "
        "    s.Push(move (new R())); "
        "    s.Push(move (new R())); "
        "    s.Push(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// OwnedArray field (owned field in a class) — §28.11 + §28.5
// ---------------------------------------------------------------------------

// §28.11 + §28.5: an `owned OwnedArray<T>` field's elements are disposed
// when the owner is disposed. Disposal of the owner triggers the field's
// backing to be freed, cascading reverse-insertion-order element disposal.
BEZEL_TEST(owned_dispose_order_598_spec_28_11,
           owned_array_field_disposes_reverse_insertion_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Item "
        "  public static _log: int := 0; "
        "  private _id: int := 0; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  destructor _log := _log * 10 + _id; end destructor; "
        "end Item; "
        "class Pool "
        "  private owned _items: OwnedArray<Item>; "
        "  public constructor() "
        "    _items := new OwnedArray<Item>(); "
        "    _items.Add(move (new Item(1))); "
        "    _items.Add(move (new Item(2))); "
        "    _items.Add(move (new Item(3))); "
        "  end constructor; "
        "end Pool; "
        "class C public static method Main(): int "
        "  do "
        "    using var p := new Pool(); "
        "  end; "
        "  return Item._log; "
        "end Main; end C;"), 321);
}
