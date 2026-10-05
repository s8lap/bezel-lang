// Regression tests for issue #2471 — an explicit `.GetEnumerator()` call
// (stdlib §4.12, the `IEnumerable<T>` ancestor via `IReadOnlyCollection<T>`)
// was non-functional for every builtin collection except `Array<T>`/
// `OwningArray<T>`: `MoveNext()` returned `false` immediately, with no
// diagnostic either way. Confirmed live on origin/main before the original
// #2471 fix for `Stack<T>`, `Queue<T>`, and `Set<T>` (LinkedList<T>/
// Dictionary<K,V> were separately confirmed too).
//
// The first #2471 fix closed the gap for `Stack<T>`/`OwningStack<T>`/
// `Queue<T>`/`OwningQueue<T>` (Stack via new reverse-order machinery,
// Queue reusing Array's forward machinery). This SECOND fix closes the
// remaining three families the issue names — `Set<T>`/`OwningSet<T>`/
// `LinkedList<T>`/`OwningLinkedList<T>`/`Dictionary<K,V>` — each of which
// has a materially different backing and was confirmed still non-functional
// on origin/main before this fix (the previously-out-of-scope guards below
// are converted here into the in-scope functional tests).
//
// Root cause (the same shape as the first fix): both the sema typing gate
// (ExprTypecheck_Calls.cpp, the #1002 block) and the codegen wrap gate
// (LlvmBackend_Calls2.cpp) matched only the previously-fixed names, so the
// explicit `.GetEnumerator()` call on these receivers fell through both
// dispatch points and landed on a zero-init fallback. Both gates now also
// include Set/OwningSet/LinkedList/OwningLinkedList/Dictionary; the wrap
// dispatch picks the backing-appropriate helper for each (Set: snapshot or
// dense-array; LinkedList: node-walking; Dictionary: entry materialisation)
// so each path's element order agrees with the `for x in coll do` desugaring
// (a separate codegen path), per the standing "the two paths must not end up
// disagreeing" direction — cross-checked directly below, not assumed from
// two independently-passing tests. The DictionaryEntry.Key/Value field
// access (LlvmBackend_Exprs.cpp) is also widened to handle a TEMPORARY
// target (`e.Current.Value` where `e.Current` is a vtable call result),
// previously only loop-variable locals were handled.
//
// Root cause (LlvmBackend_Calls2.cpp): the sema typing that marks a
// `.GetEnumerator()` CALL's result as the builtin `IEnumerator<T>` sentinel
// (ExprTypecheck_Calls.cpp, the #1002 block) was gated to `colName ==
// "Array" || colName == "OwningArray"` only; the codegen lowering that
// actually wraps the receiver into a real vtable-backed enumerator
// (`wrapArrayAsIEnumerable` + the §29.11 slot-4 clone dance) was gated
// identically (`rbn == "Array" || rbn == "OwningArray"`). Neither gate
// matched Stack/Queue, so the call fell through both dispatch points
// unresolved and unlowered, landing on a generic zero-init fallback.
//
// Fix: both gates now also include Stack/OwningStack/Queue/OwningQueue.
// Queue/OwningQueue reuse the EXISTING forward machinery unchanged — front-
// to-back (dequeue) order coincides with physical/forward order (§4.11,
// #2472's own finding), so nothing about the traversal direction needs to
// change. Stack/OwningStack route through a NEW reverse-order machinery
// (`ensureStackEnumMachinery`/`wrapStackAsIEnumerator`, LlvmBackend_Assign.cpp)
// — a structural mirror of the existing forward one (same 4-field struct
// layout, same Dispose/Current bodies) except MoveNext counts DOWN and the
// "before-first" sentinel is `bz_array_len(arr)` (a runtime value) rather
// than a fixed `-1` — so its element order agrees with #2472's now-fixed
// `for x in stack do` pop order, per the coordinator's explicit "the two
// paths must not end up disagreeing" direction. Both orders are
// cross-checked directly below, not just separately asserted.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── Stack<T> / OwningStack<T>: explicit GetEnumerator, pop order ────────

// Was: MoveNext() immediately false, n stayed 0. Now: sums correctly
// (order-insensitive — the ORDER-sensitive check is the next test).
BEZEL_TEST(stack_queue_getenumerator_2471, stack_getenumerator_now_functional_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  using var e := s.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n + e.Current; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 6);
}

// SEQUENCE-asserting (base-5 encoding, safely under the WEXITSTATUS 8-bit
// exit-code mask per tests/README.md — the exact pitfall #2472's own first
// test draft hit and corrected): pop order 3,2,1 gives 86; forward
// (bottom-to-top) order would give 38.
BEZEL_TEST(stack_queue_getenumerator_2471, stack_getenumerator_pop_order_sequence_agrees_with_2472) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  using var e := s.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n * 5 + e.Current; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// Direct cross-check that the explicit-GetEnumerator path and the for-loop
// path (#2472) produce the IDENTICAL element sequence — the coordinator's
// own "the two paths must not end up disagreeing" requirement, verified
// directly rather than inferred from two separately-passing tests.
BEZEL_TEST(stack_queue_getenumerator_2471, stack_getenumerator_order_matches_forloop_order_directly) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); s.Push(40); "
        "  using var viaEnum := new Array<int>(0); "
        "  using var e := s.GetEnumerator(); "
        "  while e.MoveNext() do viaEnum.Add(e.Current); end; "
        "  using var viaFor := new Array<int>(0); "
        "  for var x in s do viaFor.Add(x); end; "
        "  if viaEnum.Count <> viaFor.Count then return 1; end; "
        "  var i := 0; "
        "  while i < viaEnum.Count do "
        "    if viaEnum[i] <> viaFor[i] then return 2; end; "
        "    i := i + 1; "
        "  end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// `OwningStack<T>` with a class element type — same pop order, borrowing
// `Current` (non-destructive; the elements are not disposed by iteration).
BEZEL_TEST(stack_queue_getenumerator_2471, owningstack_getenumerator_pop_order_value_correct) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  using var s := new OwningStack<Res>(); "
        "  s.Push(new Res(1)); s.Push(new Res(2)); s.Push(new Res(3)); "
        "  using var e := s.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n * 5 + e.Current.Tag; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// Empty stack — MoveNext() must be false on the very first call (the
// reversed "before-first" sentinel is bz_array_len(arr) == 0, and MoveNext's
// first decrement gives -1, which fails the `>= 0` check immediately).
BEZEL_TEST(stack_queue_getenumerator_2471, empty_stack_getenumerator_movenext_false_immediately) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  using var e := s.GetEnumerator(); "
        "  return (if e.MoveNext() then 1 else 42); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// §29.11 "each call to GetEnumerator() produces an independent enumerator":
// two separate calls on the SAME Stack each replay the full pop-order
// sequence from the top — not a shared, already-exhausted cursor.
BEZEL_TEST(stack_queue_getenumerator_2471, stack_getenumerator_two_calls_each_independent_and_replay) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var n := 0; "
        "  using var e1 := s.GetEnumerator(); "
        "  while e1.MoveNext() do n := n + e1.Current; end; "
        "  using var e2 := s.GetEnumerator(); "
        "  while e2.MoveNext() do n := n + e2.Current; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 12);  // (1+2+3) + (1+2+3)
}

// ── Queue<T> / OwningQueue<T>: explicit GetEnumerator, dequeue order ────

// Was ALSO non-functional before this fix (confirmed, not assumed — Queue
// shared the exact same `colName == "Array" || "OwningArray"` gate gap as
// Stack). Front-to-back order coincides with enqueue order (reuses the
// existing forward machinery unchanged).
BEZEL_TEST(stack_queue_getenumerator_2471, queue_getenumerator_now_functional_order_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "  using var e := q.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n * 10 + e.Current; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 123);
}

BEZEL_TEST(stack_queue_getenumerator_2471, owningqueue_getenumerator_dequeue_order_value_correct) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  using var q := new OwningQueue<Res>(); "
        "  q.Enqueue(new Res(1)); q.Enqueue(new Res(2)); q.Enqueue(new Res(3)); "
        "  using var e := q.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n * 10 + e.Current.Tag; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 123);
}

// ── Set<T>/OwningSet<T>: explicit GetEnumerator now functional ──────────
// #2471 (stdlib §4.10/§4.12; language spec §29.1, v0.236): Set<T>/
// OwningSet<T>'s explicit `.GetEnumerator()` was non-functional (the sema
// typing gate and codegen wrap gate both fell through, returning a zero-init
// fallback). Now functional. Two backings are exercised:
//   * value-element Set<int> (generic-key hash table) — snapshots via
//     bz_lazy_from_set, drainLazyPipelineToOwnedIEnumerable.
//   * class-element OwningSet<Item> (dense bz_array_t) — direct forward wrap.
// Both sum to the expected total. Set iteration is insertion-ordered (§4.10),
// but sum is order-insensitive — order-sensitive coverage is added below.

// Was: MoveNext() immediately false, n stayed 0. Now: sums correctly.
BEZEL_TEST(stack_queue_getenumerator_2471, set_getenumerator_now_functional_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var st := new Set<int>(); "
        "  st.Add(5); st.Add(9); "
        "  using var e := st.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n + e.Current; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 14);  // 5 + 9
}

// value-element Set — order matches the for-loop path directly (the
// "two paths must not disagree" property, verified not assumed).
BEZEL_TEST(stack_queue_getenumerator_2471, set_getenumerator_order_matches_forloop_order_directly) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var st := new Set<int>(); "
        "  st.Add(2); st.Add(5); st.Add(8); "
        "  var ge_sum := 0; var fl_sum := 0; "
        "  using var e := st.GetEnumerator(); "
        "  while e.MoveNext() do ge_sum := ge_sum * 10 + e.Current; end; "
        "  for var x in st do fl_sum := fl_sum * 10 + x; end; "
        "  return fl_sum - ge_sum; "  // 0 iff the two paths agree exactly
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// class-element OwningSet<Item> — array-backed (the dense path),.Current.V
// reads the field of the borrowed element pointer.
BEZEL_TEST(stack_queue_getenumerator_2471, owningset_getenumerator_now_functional_value_correct) {
    const std::string program =
        "module M; class Item "
        "  public V: int := 0; "
        "  public constructor(v: int) this.V := v; end constructor; "
        "  destructor end destructor; "
        "end Item; class C public static method Main(): int "
        "  using var st := new OwningSet<Item>(); "
        "  st.Add(new Item(5)); st.Add(new Item(9)); "
        "  using var e := st.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n + e.Current.V; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 14);
}

// empty Set — MoveNext() false immediately (no elements).
BEZEL_TEST(stack_queue_getenumerator_2471, empty_set_getenumerator_movenext_false_immediately) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var st := new Set<int>(); "
        "  using var e := st.GetEnumerator(); "
        "  if e.MoveNext() then return 1; end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// ── LinkedList<T>/OwningLinkedList<T>: explicit GetEnumerator ────────────
// #2471 (stdlib §4.6/§4.12; language spec §29.1, v0.236): node-walking
// wrap (wrapLinkedListAsIEnumerable, #1069). Iteration is head-to-tail
// (§4.6: AddLast appends at the tail; literal order is head-to-tail).

// LinkedList<int> — head-to-tail order, sums to the expected total.
BEZEL_TEST(stack_queue_getenumerator_2471, linkedlist_getenumerator_now_functional_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(2); l.AddLast(3); "
        "  using var e := l.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n + e.Current; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 6);  // 1 + 2 + 3
}

// LinkedList<int> — order verifiably head-to-tail, matching the for-loop path
// (the "two paths must not disagree" property, verified directly).
BEZEL_TEST(stack_queue_getenumerator_2471, linkedlist_getenumerator_head_to_tail_order_matches_forloop) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(2); l.AddLast(3); "
        "  var ge_enc := 0; var fl_enc := 0; "
        "  using var e := l.GetEnumerator(); "
        "  while e.MoveNext() do ge_enc := ge_enc * 10 + e.Current; end; "
        "  for var x in l do fl_enc := fl_enc * 10 + x; end; "
        "  return fl_enc - ge_enc; "  // 0 iff both visit 1,2,3 in order
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// empty LinkedList — MoveNext() false immediately.
BEZEL_TEST(stack_queue_getenumerator_2471, empty_linkedlist_getenumerator_movenext_false_immediately) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  using var e := l.GetEnumerator(); "
        "  if e.MoveNext() then return 1; end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// OwningLinkedList<V> — same node-walking wrap, class-typed element. Confirms
// the GetEnumerator clone BORROWS the list (the owning list still owns its
// elements and disposes each once at its own scope end — no UAF, no double-
// free). ASan-clean on the disposal probe (probe_dispose_owningll.bzl).
BEZEL_TEST(stack_queue_getenumerator_2471, owninglinkedlist_getenumerator_now_functional_value_correct) {
    const std::string program =
        "module M; class V "
        "  public N: int := 0; "
        "  public constructor(n: int) this.N := n; end constructor; "
        "  destructor end destructor; "
        "end V; class C public static method Main(): int "
        "  using var l := new OwningLinkedList<V>(); "
        "  l.AddLast(new V(10)); l.AddLast(new V(20)); l.AddLast(new V(30)); "
        "  using var e := l.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n + e.Current.N; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 60);  // 10 + 20 + 30
}

// ── Dictionary<K,V>: explicit GetEnumerator ──────────────────────────────
// #2471 (stdlib §4.8/§4.12; language spec §29.1, v0.236): entries are
// materialised into a fresh {K, V} array (mirrors lowerDictEntryFor), then
// wrapped as an OWNING vtable-backed IEnumerable<DictionaryEntry<K,V>>.
// Iteration is insertion-ordered (§4.8). DictionaryEntry.Key/Value reads use
// the §4.8 field-access path, here widened (#2471) to handle the temporary
// shape `e.Current.Value` (e.Current is a vtable call result, not a local).

// string-key Dictionary — sum of values matches expected (insertion order).
BEZEL_TEST(stack_queue_getenumerator_2471, dictionary_getenumerator_now_functional_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"a\"] := 1; d[\"b\"] := 2; "
        "  using var e := d.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n + e.Current.Value; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 3);  // 1 + 2
}

// int-key Dictionary — exercises the generic-key path (bz_dict_keys_gen)
// and confirms the useGen branch decodes K correctly (a previous draft
// mis-flagged every non-string key as string-backed and silently mis-decoded).
BEZEL_TEST(stack_queue_getenumerator_2471, dictionary_intkey_getenumerator_keys_and_values_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d[10] := 1; d[20] := 2; d[30] := 3; "
        "  using var e := d.GetEnumerator(); "
        "  var k := 0; var v := 0; "
        "  while e.MoveNext() do k := k + e.Current.Key; v := v + e.Current.Value; end; "
        "  return k + v; "  // 60 + 6 = 66 (fits in 8 bits)
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 66);
}

// empty Dictionary — MoveNext() false immediately.
BEZEL_TEST(stack_queue_getenumerator_2471, empty_dictionary_getenumerator_movenext_false_immediately) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  using var e := d.GetEnumerator(); "
        "  if e.MoveNext() then return 1; end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}
