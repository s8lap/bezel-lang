// Codegen tests for LinkedList<T> / OwningLinkedList<T> node-based API.
// Derives from stdlib §4.6 / §4.7 (Bezel_Standard_Library_0_130.md §4.6–§4.7).
//
// Covers:
//   §4.6: AddFirst, AddLast, AddBefore, AddAfter, RemoveFirst, RemoveLast,
//         RemoveNode, Find, Count, FirstNode, LastNode, First, Last,
//         node.Value, node.Next, node.Previous
//   §4.7: OwningLinkedList<T> disposal (RemoveFirst/RemoveLast/RemoveNode/
//         Clear disposes the value exactly once; scope-end disposes all).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Wrapper that prepends a resource class R with a static disposal counter.
int runResource(const std::string& mainBody) {
    return run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;");
}
}  // namespace

// ---------------------------------------------------------------------------
// §4.6 LinkedList<T> — AddFirst / AddLast / Count
// ---------------------------------------------------------------------------

// per §4.6: AddLast appends; AddFirst prepends; Count reflects both.
BEZEL_TEST(linked_list_4_6, add_last_order_and_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); l.AddLast(30); "
        "  return l.Count; "   // 3
        "end Main; end C;"), 3);
}

BEZEL_TEST(linked_list_4_6, add_first_prepends) {
    // Verify AddFirst prepends by reading FirstNode.Value via null-guard.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(20); l.AddFirst(10); "
        "  var n := l.FirstNode; "
        "  if n = null then return -1; end; "
        "  return n.Value; "   // 10 (head)
        "end Main; end C;"), 10);
}

// per §4.6: First and Last properties return the element value.
// Access via FirstNode/LastNode node handles to satisfy nullability.
BEZEL_TEST(linked_list_4_6, first_last_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); l.AddLast(30); "
        "  var fn := l.FirstNode; "
        "  var ln := l.LastNode; "
        "  if fn = null then return -1; end; "
        "  if ln = null then return -2; end; "
        "  return fn.Value + ln.Value; "   // 10 + 30 = 40
        "end Main; end C;"), 40);
}

// ---------------------------------------------------------------------------
// §4.6: node.Value / node.Next / node.Previous
// ---------------------------------------------------------------------------

// per §4.6: AddLast returns a node; node.Value reads the element.
BEZEL_TEST(linked_list_4_6, node_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  var n := l.AddLast(42); "
        "  return n.Value; "
        "end Main; end C;"), 42);
}

// per §4.6: node.Next links to the subsequent node.
// Uses explicit null-guards on node vars (spec §7.4: nullable variable requires
// narrowing before member access).
BEZEL_TEST(linked_list_4_6, node_next_chain) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(2); l.AddLast(3); "
        "  var n := l.FirstNode; "
        "  if n = null then return -1; end; "
        "  var s := n.Value; "
        "  var n2 := n.Next; "
        "  if n2 = null then return -2; end; "
        "  s := s + n2.Value; "
        "  var n3 := n2.Next; "
        "  if n3 = null then return -3; end; "
        "  s := s + n3.Value; "
        "  return s; "   // 1 + 2 + 3 = 6
        "end Main; end C;"), 6);
}

// per §4.6: node.Previous links to the prior node.
BEZEL_TEST(linked_list_4_6, node_previous) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); "
        "  var tail := l.LastNode; "
        "  if tail = null then return -1; end; "
        "  var prev := tail.Previous; "
        "  if prev = null then return -2; end; "
        "  return prev.Value; "   // 10
        "end Main; end C;"), 10);
}

// ---------------------------------------------------------------------------
// §4.6: AddBefore / AddAfter
// ---------------------------------------------------------------------------

// per §4.6: AddAfter inserts a new node immediately after the given node.
// List: 10 → 20 → 30 after AddAfter(firstNode, 20). Count becomes 3.
BEZEL_TEST(linked_list_4_6, add_after_found_node) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(30); "
        "  var n := l.FirstNode; "
        "  if n = null then return -1; end; "
        "  l.AddAfter(n, 20); "
        // List should now be: 10 -> 20 -> 30; Count = 3
        "  return l.Count; "
        "end Main; end C;"), 3);
}

// per §4.6: AddBefore inserts immediately before the given node.
BEZEL_TEST(linked_list_4_6, add_before_found_node) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(30); "
        "  var n := l.LastNode; "
        "  l.AddBefore(n, 20); "
        // List: 10 -> 20 -> 30
        "  return l.Count; "   // 3
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// §4.6: Find — present and absent
// ---------------------------------------------------------------------------

// per §4.6: Find returns a non-null node for a present element.
BEZEL_TEST(linked_list_4_6, find_present) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); l.AddLast(30); "
        "  var n := l.Find(20); "
        "  if n = null then return 0; end; "
        "  return n.Value; "   // 20
        "end Main; end C;"), 20);
}

// per §4.6: Find returns null for an absent element.
BEZEL_TEST(linked_list_4_6, find_absent) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); "
        "  var n := l.Find(99); "
        "  if n = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// §4.6: RemoveFirst / RemoveLast / RemoveNode
// ---------------------------------------------------------------------------

// per §4.6: RemoveFirst removes the head and decrements Count.
// Reconciled for §7.11 (#486): `l.First` is `int?` (LinkedList<int>.First: T?
// per §4.6). After lifting, `l.Count * 10 + l.First` is `int?`, not `int`.
// Narrow with `?? 0`; the list is non-empty so the coalesce never fires.
BEZEL_TEST(linked_list_4_6, remove_first_decrements_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); l.AddLast(30); "
        "  l.RemoveFirst(); "
        "  return l.Count * 10 + (l.First ?? 0); "   // 2*10 + 20 = 40 (§7.11 v0.267+: no lifting over int?; unwrap with ?? per fix #3298)
        "end Main; end C;"), 40);
}

// per §4.6: RemoveLast removes the tail and decrements Count.
// Reconciled for §7.11 (#486): `l.Last` is `int?`; narrow with `?? 0`.
BEZEL_TEST(linked_list_4_6, remove_last_decrements_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); l.AddLast(30); "
        "  l.RemoveLast(); "
        "  return l.Count * 10 + (l.Last ?? 0); "   // 2*10 + 20 = 40 (§7.11: unwrap the nullable Last)
        "end Main; end C;"), 40);
}

// per §4.6: RemoveNode removes a specific node by pointer.
// Reconciled for §7.11 (#486): `l.First` and `l.Last` are `int?`; the lifted
// arithmetic chain `int + int? + int?` is `int?`. Narrow with `?? 0`.
BEZEL_TEST(linked_list_4_6, remove_node_by_pointer) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); l.AddLast(30); "
        "  var n := l.Find(20); "
        "  l.RemoveNode(n); "
        "  return l.Count * 10 + (l.First ?? 0) + (l.Last ?? 0); "   // 2*10 + 10 + 30 = 60 (§7.11: unwrap both)
        "end Main; end C;"), 60);
}

// ---------------------------------------------------------------------------
// §4.6: FirstNode / LastNode properties return node handles.
// ---------------------------------------------------------------------------

// per §4.6: FirstNode/LastNode return node handles; access .Value after null-guard.
BEZEL_TEST(linked_list_4_6, first_node_last_node) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); "
        "  var fn := l.FirstNode; "
        "  var ln := l.LastNode; "
        "  if fn = null then return -1; end; "
        "  if ln = null then return -2; end; "
        "  return fn.Value * 10 + ln.Value; "   // 10*10 + 20 = 120
        "end Main; end C;"), 120);
}

// ---------------------------------------------------------------------------
// §4.7: OwningLinkedList<T> — disposal semantics
// ---------------------------------------------------------------------------

// per §4.7: OwningLinkedList.RemoveFirst disposes the removed element exactly once;
// scope-end disposes the remaining elements.
// Encoded: afterRemove*10 + afterScopeEnd (counts of disposals).
BEZEL_TEST(owned_linked_list_4_7, remove_first_disposes_element) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterRemove := -1; "
        "do "
        "  using var l := new OwningLinkedList<R>(); "
        "  l.AddLast(move (new R())); "
        "  l.AddLast(move (new R())); "
        "  l.AddLast(move (new R())); "
        "  l.RemoveFirst(); "
        "  afterRemove := R._n; "   // 1 disposed
        "end; "
        "return afterRemove * 10 + R._n;"),   // 1*10 + 3 = 13
        13);
}

// per §4.7: OwningLinkedList.RemoveLast disposes the removed element exactly once.
BEZEL_TEST(owned_linked_list_4_7, remove_last_disposes_element) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterRemove := -1; "
        "do "
        "  using var l := new OwningLinkedList<R>(); "
        "  l.AddLast(move (new R())); "
        "  l.AddLast(move (new R())); "
        "  l.RemoveLast(); "
        "  afterRemove := R._n; "   // 1 disposed
        "end; "
        "return afterRemove * 10 + R._n;"),   // 1*10 + 2 = 12
        12);
}

// per §4.7: OwningLinkedList.RemoveNode disposes the targeted element exactly once.
BEZEL_TEST(owned_linked_list_4_7, remove_node_disposes_element) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterRemove := -1; "
        "do "
        "  using var l := new OwningLinkedList<R>(); "
        "  l.AddLast(move (new R())); "
        "  l.AddLast(move (new R())); "
        "  l.AddLast(move (new R())); "
        "  var fn := l.FirstNode; "
        "  if fn = null then return -1; end; "
        "  var n := fn.Next; "
        "  if n = null then return -2; end; "
        "  l.RemoveNode(n); "
        "  afterRemove := R._n; "   // 1 disposed
        "end; "
        "return afterRemove * 10 + R._n;"),   // 1*10 + 3 = 13
        13);
}

// per §4.7: scope-end of OwningLinkedList<T> disposes ALL remaining elements.
BEZEL_TEST(owned_linked_list_4_7, scope_end_disposes_all) {
    BEZEL_EXPECT_EQ(runResource(
        "do "
        "  using var l := new OwningLinkedList<R>(); "
        "  l.AddLast(move (new R())); "
        "  l.AddLast(move (new R())); "
        "  l.AddLast(move (new R())); "
        "end; "
        "return R._n;"),   // 3 disposed
        3);
}

// per §4.7: borrowing LinkedList<T> does NOT dispose elements on scope-end.
BEZEL_TEST(linked_list_4_6, borrowing_list_does_not_dispose_elements) {
    BEZEL_EXPECT_EQ(runResource(
        "var r1 := new R(); "
        "var r2 := new R(); "
        "do "
        "  using var l := new LinkedList<R>(); "
        "  l.AddLast(r1); l.AddLast(r2); "
        "end; "
        // LinkedList<R> scope-end should NOT dispose r1/r2.
        "var disposed_in_list_scope := R._n; "
        "r1.Dispose(); r2.Dispose(); "
        "return disposed_in_list_scope;"),   // 0 (list did not dispose)
        0);
}

// per §4.7: OwningLinkedList.Clear disposes every element exactly once.
BEZEL_TEST(owned_linked_list_4_7, clear_disposes_all_elements) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterClear := -1; "
        "do "
        "  using var l := new OwningLinkedList<R>(); "
        "  l.AddLast(move (new R())); "
        "  l.AddLast(move (new R())); "
        "  l.AddLast(move (new R())); "
        "  l.Clear(); "
        "  afterClear := R._n; "   // all 3 disposed
        "end; "
        "return afterClear;"),
        3);
}

// per §4.6: Count is correct after multiple operations.
BEZEL_TEST(linked_list_4_6, count_after_operations) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(2); l.AddLast(3); "
        "  l.RemoveFirst(); l.RemoveLast(); "
        "  return l.Count; "   // 1
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// fix #450: LINQ aggregate/terminal operators on LinkedList<T> — stdlib §5 / §4.6
// LinkedList<T> implements IEnumerable<T> (§4.6); LINQ aggregates must enumerate
// nodes head-to-tail via bz_lazy_from_llist, NOT treat the list pointer as a
// bz_array_t* (which misread bz_llist_t.tail as bz_array_t.count → garbage/0).
// ---------------------------------------------------------------------------

// §5 + §4.6 fix #450: Sum on a LinkedList<int> returns the correct total.
BEZEL_TEST(linked_list_4_6, linq_sum_spec_5_fix450) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); l.AddLast(30); "
        "  return l.Sum(lambda(x: int) => x); "   // 10+20+30 = 60
        "end Main; end C;"), 60);
}

// §5 + §4.6 fix #450: Min on a LinkedList<int> returns the minimum element.
BEZEL_TEST(linked_list_4_6, linq_min_spec_5_fix450) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(5); l.AddLast(2); l.AddLast(8); l.AddLast(1); "
        "  return l.Min(lambda(x: int) => x); "   // min = 1
        "end Main; end C;"), 1);
}

// §5 + §4.6 fix #450: Max on a LinkedList<int> returns the maximum element.
BEZEL_TEST(linked_list_4_6, linq_max_spec_5_fix450) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(5); l.AddLast(2); l.AddLast(8); l.AddLast(1); "
        "  return l.Max(lambda(x: int) => x); "   // max = 8
        "end Main; end C;"), 8);
}

// §5 + §4.6 fix #450: Count(pred) on a LinkedList<int> counts matching elements.
// fix #3342 (§26.5 rule 1 / stdlib §4.4, v0.277): `LinkedList<T>` declares
// `Count` as a 0-arg property, so the direct `l.Count(pred)` shape is a
// compile error — the §5 LINQ operator is unreachable by name. The composed
// spelling `l.Where(pred).Count()` (Where is shadowed nowhere) reaches the
// operator pipeline.
BEZEL_TEST(linked_list_4_6, linq_count_pred_spec_5_fix450) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(2); l.AddLast(3); l.AddLast(4); l.AddLast(5); "
        "  return l.Where(lambda(x: int) => x mod 2 = 0).Count(); "  // even: 2,4 → 2
        "end Main; end C;"), 2);
}

// §5 + §4.6 fix #450: Any(pred) on a LinkedList<int> — true when a match exists.
BEZEL_TEST(linked_list_4_6, linq_any_pred_spec_5_fix450) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(3); l.AddLast(5); "
        "  return int(l.Any(lambda(x: int) => x > 4)); "  // 5 > 4 → true → 1
        "end Main; end C;"), 1);
}

// §5 + §4.6 fix #450: regression — Sum on a single-element LinkedList.
BEZEL_TEST(linked_list_4_6, linq_sum_single_element_spec_5_fix450) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(42); "
        "  return l.Sum(lambda(x: int) => x); "  // 42
        "end Main; end C;"), 42);
}

// #728 / §4.13 / §29.2: `for var x in <LinkedList<T>>` iterates the node chain.
// Previously emitted "not yet implemented: codegen for this for statement"
// (LinkedList is not array-backed, so lowerArrayFor excluded it). lowerLinkedListFor
// now walks the runtime node cursor.
BEZEL_TEST(codegen_linked_list, for_in_sum_728) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<int>(); "
        "  ll.AddLast(10); ll.AddLast(20); ll.AddLast(30); "
        "  var sum := 0; "
        "  for var x in ll do sum := sum + x; end; "
        "  return sum; "  // 60
        "end Main; end C;"), 60);
}

// #728: break works inside the LinkedList for-in (loopStack_ wired).
BEZEL_TEST(codegen_linked_list, for_in_break_728) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<int>(); "
        "  ll.AddLast(1); ll.AddLast(2); ll.AddLast(3); ll.AddLast(4); "
        "  var sum := 0; "
        "  for var x in ll do "
        "    if x = 3 then break; end; "
        "    sum := sum + x; "
        "  end; "
        "  return sum; "  // 1+2 = 3
        "end Main; end C;"), 3);
}

// #728: string-element LinkedList — the loop var binds the element value.
BEZEL_TEST(codegen_linked_list, for_in_string_elements_728) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<string>(); "
        "  ll.AddLast(\"ab\"); ll.AddLast(\"cde\"); "
        "  var total := 0; "
        "  for var s in ll do total := total + s.Length; end; "
        "  return total; "  // 2 + 3 = 5
        "end Main; end C;"), 5);
}

// #738 / §4.6 / §15.4: a LinkedList<T> COLLECTION LITERAL. LinkedList is
// bz_llist_t-backed (not bz_array_t), and isCollectionTypeName excludes it, so
// the literal previously fell through to the inline-array path — the EMPTY `[]`
// crashed (free() of an invalid pointer) and a non-empty literal IR-failed.
// materializeHeapCollectionLiteral now builds it via bz_llist_alloc/add_last.
BEZEL_TEST(codegen_linked_list, empty_literal_738) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll: LinkedList<int> := []; "
        "  return ll.Count; "  // 0, no crash
        "end Main; end C;"), 0);
}
BEZEL_TEST(codegen_linked_list, nonempty_literal_count_738) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll: LinkedList<int> := [42]; "
        "  return ll.Count; "  // 1
        "end Main; end C;"), 1);
}
BEZEL_TEST(codegen_linked_list, literal_iterates_in_order_738) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll: LinkedList<int> := [10, 20, 30]; "
        "  var s := 0; "
        "  for var x in ll do s := s + x; end; "
        "  return s + ll.Count; "  // 60 + 3 = 63
        "end Main; end C;"), 63);
}
// Migrated from OwningLinkedList<int> → LinkedList<int> per stdlib §4.2/#1327:
// owned collections require class element types; for value-type iteration
// use the non-owning LinkedList<T>.
BEZEL_TEST(codegen_linked_list, owned_literal_738) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll: LinkedList<int> := [5, 7]; "
        "  var s := 0; "
        "  for var x in ll do s := s + x; end; "
        "  return s + ll.Count; "  // 12 + 2 = 14
        "end Main; end C;"), 14);
}
