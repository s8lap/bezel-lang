// Regression tests for issue #2471 — an explicit `.GetEnumerator()` call
// (stdlib §4.12, the `IEnumerable<T>` ancestor via `IReadOnlyCollection<T>`)
// was non-functional for every builtin collection except `Array<T>`/
// `OwningArray<T>`: `MoveNext()` returned `false` immediately, with no
// diagnostic either way. Confirmed live on origin/main before this fix for
// `Stack<T>`, `Queue<T>`, and `Set<T>` (LinkedList<T>/Dictionary<K,V> were
// separately confirmed too — see below — closing the "inferred, not
// confirmed" gap #2457 slice 2's own filing of #2471 left open).
//
// This fix closes the gap for exactly `Stack<T>`/`OwningStack<T>`/
// `Queue<T>`/`OwningQueue<T>` — the two families #2457's own slices audit —
// per the coordinator's direction to attempt #2471 next after #2472, with
// disposal semantics checked against the existing `wrapArrayAsIEnumerable`
// pattern (not re-invented) and Queue's order confirmed to already coincide
// with the existing forward machinery Array/OwningArray use.
// `Set<T>`/`LinkedList<T>`/`Dictionary<K,V>` (and their Owning variants)
// remain open, confirmed still non-functional below as an explicit
// regression guard against scope creep.
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
        "  if viaEnum.Length <> viaFor.Length then return 1; end; "
        "  var i := 0; "
        "  while i < viaEnum.Length do "
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

// ── Out-of-scope receivers remain UNCHANGED (no scope creep) ────────────

// Set<T>/LinkedList<T>/Dictionary<K,V> are NOT part of this fix — confirmed
// (not just inferred) still non-functional, guarding against an accidental
// widening of the gate condition beyond Stack/Queue.
BEZEL_TEST(stack_queue_getenumerator_2471, set_getenumerator_still_out_of_scope_unaffected) {
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
    BEZEL_EXPECT_EQ(run(program), 0);  // still non-functional; tracked separately
}

BEZEL_TEST(stack_queue_getenumerator_2471, linkedlist_getenumerator_still_out_of_scope_unaffected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(2); "
        "  using var e := l.GetEnumerator(); "
        "  var n := 0; "
        "  while e.MoveNext() do n := n + e.Current; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);  // still non-functional; tracked separately
}

BEZEL_TEST(stack_queue_getenumerator_2471, dictionary_getenumerator_still_out_of_scope_unaffected) {
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
    BEZEL_EXPECT_EQ(run(program), 0);  // still non-functional; tracked separately
}
