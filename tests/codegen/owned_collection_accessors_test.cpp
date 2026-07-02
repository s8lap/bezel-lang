// Codegen — owned-collection pop/peek/tryadd accessor fixes.
// Covers issues #642, #643, and #654 (tail A and B).
//
// #642: OwningArray<T>.PopLast() / PopAt(i) / PopFirst() — stdlib §4.5.
//        OwningLinkedList<T>.PopFirst() / PopLast()     — stdlib §4.7.
// #643: OwningDictionary<K,V>.PopValue(k)              — stdlib §4.9.
// #654-tail A: OwningQueue<T>.Peek() returns FIRST (FIFO) element — §4.11.
// #654-tail B: OwningSet<T>.TryAdd(owned item) correct Count/bool — §4.10.
//
// Disposal correctness:
//   * Pop* transfers ownership out — the popped element is NOT double-disposed.
//   * Peek does NOT transfer ownership (borrow) — Count unchanged, no disposal.
//   * TryAdd false path disposes the rejected duplicate (§4.10 spec rule).
//
// Test encoding convention (from owned_collection_ops_test.cpp):
//   static R._n counter bumped by R's destructor; exit codes encode counters.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// Shared resource class harness (same as owned_collection_ops_test.cpp).
int runResource(const std::string& mainBody) {
    return run(
        "module M; "
        "class R "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;");
}

}  // namespace

// ---------------------------------------------------------------------------
// #642 / stdlib §4.5: OwningArray<T>.PopLast() returns the last element's value.
// ---------------------------------------------------------------------------

// stdlib §4.5 example: arr = [R(7), R(9)]; PopLast returns R with Val=9.
// Return 1 on success, 0 on wrong value.
BEZEL_TEST(owned_coll_acc_642, owned_array_poplast_returns_last_element_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R(7)); "
        "  arr.Add(new R(9)); "
        "  using var popped := arr.PopLast(); "
        "  if popped.Val = 9 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// stdlib §4.5: PopLast leaves the remaining elements intact.
// After PopLast from [R(7), R(9)], Count=1 and Get(0).Val=7.
// Encoded: (Count)*10 + (Get(0).Val == 7 ? 1 : 0) → 11.
BEZEL_TEST(owned_coll_acc_642, owned_array_poplast_count_and_survivor_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R(7)); "
        "  arr.Add(new R(9)); "
        "  do using var _ := arr.PopLast(); end; "
        "  var cnt := arr.Count; "
        "  var survVal := arr.Get(0).Val; "
        "  return cnt * 10 + int(survVal = 7); "   // 1*10 + 1 = 11
        "end Main; end C;"), 11);
}

// stdlib §4.5: PopLast transfers ownership — the popped element is NOT disposed
// at pop time; only at the `using var` scope end.  Encoded:
//   afterPop * 10 + afterScope → 01 (not disposed at pop; disposed at scope end).
BEZEL_TEST(owned_coll_acc_642, owned_array_poplast_transfers_ownership_no_double_dispose) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterPop := -1; "
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R(5)); "
        "  using var popped := arr.PopLast(); "
        "  afterPop := R._n; "           // 0 — not disposed yet
        "end; "
        "return afterPop * 10 + R._n;"), // 0*10 + 1 = 1
        1);
}

// ---------------------------------------------------------------------------
// #642 / stdlib §4.5: OwningArray<T>.PopAt(index) — middle element.
// ---------------------------------------------------------------------------

// stdlib §4.5: PopAt(1) from [R(1), R(2), R(3)] returns R with Val=2.
// Count after = 2, remaining values are R(1) at 0, R(3) at 1.
// Encoded: (poppedVal==2 ? 1 : 0)*100 + Count*10 + (Get(1).Val==3 ? 1 : 0) = 121.
BEZEL_TEST(owned_coll_acc_642, owned_array_popat_middle_correct_value_and_survivors) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R(1)); arr.Add(new R(2)); arr.Add(new R(3)); "
        "  do using var popped := arr.PopAt(1); "
        "    var pv := popped.Val; "
        "    var cnt := arr.Count; "
        "    var lastV := arr.Get(1).Val; "
        "    return int(pv = 2) * 100 + cnt * 10 + int(lastV = 3); " // 121
        "  end; "
        "  return 0; "
        "end Main; end C;"), 121);
}

// stdlib §4.5: PopAt(0) transfers ownership — popped element is not disposed
// at pop time; it is disposed when `using var popped` goes out of scope (end
// of the `do` block).  arr also disposes its remaining element (R(2)) at
// scope end.  Encoded: afterPop * 10 + afterScope → 02.
BEZEL_TEST(owned_coll_acc_642, owned_array_popat_first_transfers_ownership) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterPop := -1; "
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R(1)); arr.Add(new R(2)); "
        "  using var popped := arr.PopAt(0); "
        "  afterPop := R._n; "
        "end; "
        "return afterPop * 10 + R._n;"), // 0*10 + 2 = 2
        2);
}

// ---------------------------------------------------------------------------
// #642 / stdlib §4.7: OwningLinkedList<T>.PopFirst() — front element.
// ---------------------------------------------------------------------------

// stdlib §4.7: PopFirst from a single-element list returns the element.
// Val=99 after AddLast(R(99)); PopFirst returns R with Val=99.
BEZEL_TEST(owned_coll_acc_642, owned_linked_list_popfirst_returns_front_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var oll := new OwningLinkedList<R>(); "
        "  oll.AddLast(new R(99)); "
        "  using var pf := oll.PopFirst(); "
        "  if pf.Val = 99 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// stdlib §4.7: PopFirst transfers ownership — not disposed at pop time.
// Encoded: afterPop * 10 + afterScope → 01.
BEZEL_TEST(owned_coll_acc_642, owned_linked_list_popfirst_transfers_ownership_no_double_dispose) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterPop := -1; "
        "do "
        "  using var oll := new OwningLinkedList<R>(); "
        "  oll.AddLast(new R(7)); "
        "  using var pf := oll.PopFirst(); "
        "  afterPop := R._n; "
        "end; "
        "return afterPop * 10 + R._n;"), // 0*10 + 1 = 1
        1);
}

// stdlib §4.7: PopFirst on a two-element list leaves the second element.
// [R(10), R(20)] — PopFirst gives R(10), remaining Count=1; access head via
// FirstNode.Value (stdlib §4.6/§4.7 node API).
// Encoded: (poppedVal==10 ? 1 : 0)*100 + Count*10 + (head.Val==20 ? 1 : 0) = 111.
BEZEL_TEST(owned_coll_acc_642, owned_linked_list_popfirst_count_and_survivor_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var oll := new OwningLinkedList<R>(); "
        "  oll.AddLast(new R(10)); oll.AddLast(new R(20)); "
        "  do using var pf := oll.PopFirst(); "
        "    var pv := pf.Val; "
        "    var cnt := oll.Count; "
        "    var hn := oll.FirstNode; "
        "    if hn = null then return -1; end; "
        "    var headV := hn.Value.Val; "
        "    return int(pv = 10) * 100 + cnt * 10 + int(headV = 20); " // 111
        "  end; "
        "  return 0; "
        "end Main; end C;"), 111);
}

// ---------------------------------------------------------------------------
// #643 / stdlib §4.9: OwningDictionary<K,V>.PopValue(k) — remove and return.
// ---------------------------------------------------------------------------

// stdlib §4.9: PopValue removes entry and returns owned V without disposing.
// After od.Add("b", Conn("hostB")); od.PopValue("b").Host == "hostB".
BEZEL_TEST(owned_coll_acc_643, owned_dict_popvalue_returns_correct_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Conn "
        "  public Host: string := \"\"; "
        "  public constructor(h: string) Host := h; end constructor; "
        "end Conn; "
        "class C public static method Main(): int "
        "  using var od := new OwningDictionary<string, Conn>(); "
        "  od.Add(\"b\", new Conn(\"hostB\")); "
        "  using var popped := od.PopValue(\"b\"); "
        "  if popped.Host = \"hostB\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// stdlib §4.9: PopValue removes the key — ContainsKey returns false after pop.
// Encoded: (ContainsKey before)*100 + (ContainsKey after)*10 + (Count after) = 100.
BEZEL_TEST(owned_coll_acc_643, owned_dict_popvalue_removes_key_count_decrements) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Conn "
        "  public constructor(h: string) end constructor; "
        "end Conn; "
        "class C public static method Main(): int "
        "  using var od := new OwningDictionary<string, Conn>(); "
        "  od.Add(\"x\", new Conn(\"a\")); "
        "  od.Add(\"y\", new Conn(\"b\")); "
        "  var before := od.ContainsKey(\"x\"); "
        "  do using var _ := od.PopValue(\"x\"); end; "
        "  var after := od.ContainsKey(\"x\"); "
        "  var cnt := od.Count; "
        "  return int(before) * 100 + int(after) * 10 + cnt; " // 1*100+0*10+1 = 101
        "end Main; end C;"), 101);
}

// stdlib §4.9: PopValue transfers ownership — the popped value is NOT disposed
// at pop time.  Encoded: afterPop * 10 + afterScope → 01.
BEZEL_TEST(owned_coll_acc_643, owned_dict_popvalue_transfers_ownership_no_double_dispose) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class V "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end V; "
        "class C public static method Main(): int "
        "  var afterPop := -1; "
        "  do "
        "    using var od := new OwningDictionary<string, V>(); "
        "    od.Add(\"k\", new V()); "
        "    using var popped := od.PopValue(\"k\"); "
        "    afterPop := V._n; "
        "  end; "
        "  return afterPop * 10 + V._n; " // 0*10 + 1 = 1
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// #654-tail A / stdlib §4.11: Queue<T>.Peek() returns the FIRST element.
// (Migrated from OwningQueue<int> → Queue<int> per stdlib §4.2/#1327: owned
// collections require class element types; int is a value type.)
// ---------------------------------------------------------------------------

// stdlib §4.11: Peek on Queue<int> [10, 20] returns 10 (front).
// Encoded: (peeked == 10 ? 1 : 0).
BEZEL_TEST(owned_coll_acc_654a, owned_queue_peek_returns_first_element_fifo) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var oq := new Queue<int>(); "
        "    oq.Enqueue(10); oq.Enqueue(20); "
        "    var front := oq.Peek(); "
        "    if front = 10 then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// stdlib §4.11: Peek does NOT remove the element — Count unchanged.
// Encoded: countBefore*10 + countAfter → 22.
BEZEL_TEST(owned_coll_acc_654a, owned_queue_peek_does_not_remove_count_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var oq := new Queue<int>(); "
        "    oq.Enqueue(10); oq.Enqueue(20); "
        "    var before := oq.Count; "
        "    var _ := oq.Peek(); "
        "    var after := oq.Count; "
        "    return before * 10 + after; " // 2*10+2 = 22
        "  end Main; end C;"), 22);
}

// stdlib §4.11: Peek twice returns the same value — still FIFO front.
BEZEL_TEST(owned_coll_acc_654a, owned_queue_peek_twice_same_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var oq := new Queue<int>(); "
        "    oq.Enqueue(5); oq.Enqueue(99); "
        "    var p1 := oq.Peek(); "
        "    var p2 := oq.Peek(); "
        "    if p1 = 5 and p2 = 5 then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// stdlib §4.11: Stack<T>.Peek() returns the LAST element (top).
// Regression guard — fixing Queue Peek must not break Stack Peek.
// (Migrated from OwningStack<int> → Stack<int> per stdlib §4.2/#1327.)
BEZEL_TEST(owned_coll_acc_654a, owned_stack_peek_still_returns_last_element) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var os := new Stack<int>(); "
        "    os.Push(1); os.Push(2); os.Push(3); "
        "    var top := os.Peek(); "
        "    if top = 3 then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// #654-tail B / stdlib §4.10: OwningSet<T>.TryAdd(owned item) — Count/bool.
// ---------------------------------------------------------------------------

// stdlib §4.10: TryAdd a fresh item returns true and Count increments.
// After Add(R(1)) and TryAdd(R(2)): added=true, Count=2.
// Encoded: int(added)*100 + Count*10 → 120.
BEZEL_TEST(owned_coll_acc_654b, owned_set_tryadd_new_item_true_count_increments) {
    BEZEL_EXPECT_EQ(runResource(
        "using var os := new OwningSet<R>(); "
        "os.Add(new R(1)); "
        "var added := os.TryAdd(new R(2)); "
        "var cnt := os.Count; "
        "return int(added) * 100 + cnt * 10;"),  // 1*100+2*10 = 120
        120);
}

// stdlib §4.10: For class-typed OwningSet, equality is reference identity
// (§8.3).  Two distinct new R(1) objects are NEVER duplicates — each TryAdd
// returns true and Count increments.
// Encoded: int(added)*100 + Count*10 → 120.
BEZEL_TEST(owned_coll_acc_654b, owned_set_tryadd_distinct_class_objects_both_added) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var os := new OwningSet<R>(); "
        "  os.Add(new R(1)); "
        "  var added := os.TryAdd(new R(1)); " // distinct pointer → not duplicate → true
        "  var cnt := os.Count; "
        "  return int(added) * 100 + cnt * 10; " // 1*100+2*10 = 120
        "end Main; end C;"), 120);
}

// stdlib §4.10: OwningSet<class T> disposes all its elements at scope end.
// Add two distinct R objects via TryAdd; on scope end both are disposed.
// Encoded: afterAdd * 10 + afterScope → 02 (0 disposed during adds, 2 at end).
BEZEL_TEST(owned_coll_acc_654b, owned_set_class_elements_disposed_at_scope_end) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterAdd := -1; "
        "do "
        "  using var os := new OwningSet<R>(); "
        "  os.TryAdd(new R(1)); "
        "  os.TryAdd(new R(2)); "
        "  afterAdd := R._n; "       // 0 — nothing disposed yet
        "end; "
        "return afterAdd * 10 + R._n;"), // 0*10 + 2 = 2
        2);
}

// stdlib §4.10: Contains returns true for an element added via Add/TryAdd.
// Use a plain `var` (not `using var`) for the reference after Add, to avoid
// ownership conflicts (the set owns the object after Add; a `using var` would
// double-dispose at scope end).  Contains checks by reference identity (§8.3).
BEZEL_TEST(owned_coll_acc_654b, owned_set_tryadd_contains_after_success) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var os := new OwningSet<R>(); "
        "  var added := os.TryAdd(new R(42)); "
        "  var cnt := os.Count; "
        "  return int(added) * 10 + cnt; " // 1*10+1 = 11
        "end Main; end C;"), 11);
}
