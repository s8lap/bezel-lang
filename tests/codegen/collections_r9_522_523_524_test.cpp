// Codegen regression — three collection bugs (issues #522, #523, #524).
// Spec citations:
//   §4.10 Set<T> / OwningSet<T> stdlib:  TryAdd(item: T): bool
//   §4.11 Stack<T> stdlib:              Pop(): T // raises if empty
//   §4.12 Queue<T> stdlib:              Dequeue(): T // raises if empty
//   §17.5 (in-operator):               c in s ≡ s.Contains(c)
//   §4.8 (in-operator on Dictionary):  key in dict ≡ dict.ContainsKey(key)
//
// Each test group demonstrates the broken behaviour first (what the bug
// produced) and the correct behaviour required by the spec.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// #522 — Set<T>.TryAdd broken two ways
// §4.10 stdlib: "public method TryAdd(item: T): bool; // returns whether added"
// ===========================================================================

// §4.10: TryAdd inserts the element and returns true when it was newly added.
// Before the fix: element was NOT inserted → s.Count remained 0 (broken path (a)).
BEZEL_TEST(collections_r9_522, tryadd_inserts_element_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  var added := s.TryAdd(5); "    // should insert + return true
        "  return s.Count; "              // must be 1 (was 0 before fix)
        "end Main; end C;"), 1);
}

// §4.10: TryAdd returns true when the element is newly added.
// Distinct from the insertion test above (checks the return value, not Count).
BEZEL_TEST(collections_r9_522, tryadd_returns_true_when_newly_added_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  return int(s.TryAdd(5)); "     // true (1) for a new element
        "end Main; end C;"), 1);
}

// §4.10: Second TryAdd with the same element returns false (already present).
// Set deduplication: adding 5 twice → Count stays 1; second TryAdd returns false.
// Split into two assertions to avoid bool-arithmetic widening issues.
BEZEL_TEST(collections_r9_522, tryadd_returns_false_for_duplicate_spec_4_10) {
    // Count stays 1 after two TryAdds of the same value.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.TryAdd(5); "                 // first: adds
        "  s.TryAdd(5); "                 // second: already present (idempotent)
        "  return s.Count; "              // 1
        "end Main; end C;"), 1);
    // Second TryAdd returns false (element was already present).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.TryAdd(5); "
        "  var dup := s.TryAdd(5); "      // already present → false
        "  if dup then return 0; end; "   // would be true if broken
        "  return 1; "                    // 1 means dup was false (correct)
        "end Main; end C;"), 1);
}

// §4.10: TryAdd return type is bool (i1) so it can be used as a branch
// condition.  Before the fix codegen emitted i32 → IR verifier failure
// "Branch condition is not i1 type! br i32" (broken path (b)).
BEZEL_TEST(collections_r9_522, tryadd_branch_condition_is_i1_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  var r := 0; "
        "  if s.TryAdd(7) then r := 1; end; "   // must not IR-fail
        "  return r; "                           // 1 (element added)
        "end Main; end C;"), 1);
}

// §4.10: TryAdd + Add interoperability: Add then TryAdd on same element.
// Add is idempotent; TryAdd on an element already present via Add → false.
BEZEL_TEST(collections_r9_522, tryadd_after_add_returns_false_spec_4_10) {
    // Count stays 1.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(42); "                   // insert via Add
        "  s.TryAdd(42); "                // duplicate (already present)
        "  return s.Count; "              // 1
        "end Main; end C;"), 1);
    // TryAdd returns false.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(42); "
        "  var added := s.TryAdd(42); "   // duplicate → false
        "  if added then return 0; end; " // would be 0 if broken (was true)
        "  return 1; "                    // 1 means added was false (correct)
        "end Main; end C;"), 1);
}

// §4.10: TryAdd on a Set<string> (string key, not generic key). Checks that
// string-keyed Sets also work correctly with TryAdd.
BEZEL_TEST(collections_r9_522, tryadd_string_set_spec_4_10) {
    // Count after adding "hello" twice and "world" once should be 2.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<string>(); "
        "  s.TryAdd(\"hello\"); "   // new
        "  s.TryAdd(\"hello\"); "   // dup (idempotent)
        "  s.TryAdd(\"world\"); "   // new
        "  return s.Count; "        // 2
        "end Main; end C;"), 2);
    // First TryAdd returns true.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<string>(); "
        "  var a1 := s.TryAdd(\"hello\"); "  // new → true
        "  if a1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
    // Second TryAdd on same element returns false.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<string>(); "
        "  s.TryAdd(\"hello\"); "
        "  var a2 := s.TryAdd(\"hello\"); "  // dup → false
        "  if a2 then return 0; end; "        // should NOT enter here
        "  return 1; "                        // 1 means a2 was false (correct)
        "end Main; end C;"), 1);
}

// ===========================================================================
// #523 — Stack.Pop() / Queue.Dequeue() on empty must raise
// §4.11 stdlib: "Pop(): T // raises if empty"
// §4.12 stdlib: "Dequeue(): T // raises if empty"
// ===========================================================================

// §4.11: Pop() on an empty Stack raises InvalidOperationException.
// Before the fix: silently returned 0 (wrong).
BEZEL_TEST(collections_r9_523, stack_pop_empty_raises_spec_4_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  try "
        "    var x := s.Pop(); "          // must raise on empty
        "    return -1; "                 // unreachable
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return -2; "
        "end Main; end C;"), 42);
}

// §4.12: Dequeue() on an empty Queue raises InvalidOperationException.
// Before the fix: silently returned 0 (wrong).
BEZEL_TEST(collections_r9_523, queue_dequeue_empty_raises_spec_4_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  try "
        "    var x := q.Dequeue(); "      // must raise on empty
        "    return -1; "                 // unreachable
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return -2; "
        "end Main; end C;"), 42);
}

// §4.11 CONTROL: non-empty Stack.Pop() returns the top (LIFO) and Count drops.
// This verifies the raise-guard is skipped on non-empty stacks (no regression).
BEZEL_TEST(collections_r9_523, stack_pop_nonempty_returns_top_spec_4_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var top := s.Pop(); "          // must be 3 (LIFO)
        "  return top * 10 + s.Count; "  // 3*10 + 2 = 32
        "end Main; end C;"), 32);
}

// §4.12 CONTROL: non-empty Queue.Dequeue() returns the front (FIFO) and Count drops.
BEZEL_TEST(collections_r9_523, queue_dequeue_nonempty_returns_front_spec_4_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(10); q.Enqueue(20); q.Enqueue(30); "
        "  var front := q.Dequeue(); "    // must be 10 (FIFO)
        "  return front + q.Count; "     // 10 + 2 = 12
        "end Main; end C;"), 12);
}

// §4.11: Peek() on empty raises InvalidOperationException (same spec rule as Pop).
// stdlib §4.11: "Peek(): T // raises if empty"
BEZEL_TEST(collections_r9_523, stack_peek_empty_raises_spec_4_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  try "
        "    var x := s.Peek(); "         // must raise on empty
        "    return -1; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return -2; "
        "end Main; end C;"), 42);
}

// §4.11 CONTROL: Peek() on non-empty returns top WITHOUT removing it.
BEZEL_TEST(collections_r9_523, stack_peek_nonempty_does_not_remove_spec_4_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(5); s.Push(7); "
        "  var top := s.Peek(); "         // 7 (top), not removed
        "  return top * 10 + s.Count; "  // 7*10 + 2 = 72
        "end Main; end C;"), 72);
}

// ===========================================================================
// #524 — `in` on Set<T> and Dictionary<K,V> always returned false
// §17.5: c in s ≡ s.Contains(c)
// §4.8:  key in dict ≡ dict.ContainsKey(key)
// ===========================================================================

// §17.5: `5 in s` is true after s.Add(5) and false for an absent element.
// Before the fix: always returned false (the `in` operator was not routing to
// Contains for Set receivers).
BEZEL_TEST(collections_r9_524, set_in_operator_present_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(5); s.Add(10); "
        "  return int(5 in s); "          // true (1) — was false before fix
        "end Main; end C;"), 1);
}

BEZEL_TEST(collections_r9_524, set_in_operator_absent_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(5); "
        "  return int(6 in s); "          // false (0)
        "end Main; end C;"), 0);
}

// §17.5: combined present/absent check — both 5 present and 6 absent.
BEZEL_TEST(collections_r9_524, set_in_operator_combined_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(5); s.Add(10); "
        "  var r := 0; "
        "  if 5 in s then r := r + 10; end; "   // present → +10
        "  if not (6 in s) then r := r + 1; end; "  // absent → +1
        "  return r; "   // 11
        "end Main; end C;"), 11);
}

// §4.8: `key in dict` ≡ dict.ContainsKey(key). String key.
// Before the fix: always returned false even after d.Add("k", 99).
BEZEL_TEST(collections_r9_524, dict_in_operator_string_key_spec_4_8) {
    // Present key returns true.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"k\", 99); "
        "  if \"k\" in d then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
    // Absent key returns false.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"k\", 99); "
        "  if \"x\" in d then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §4.8: `key in dict` with int key (generic-key path).
BEZEL_TEST(collections_r9_524, dict_in_operator_int_key_spec_4_8) {
    // Present key.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Add(42, 100); "
        "  if 42 in d then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
    // Absent key.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Add(42, 100); "
        "  if 99 in d then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// CONTROL §17.5: Range `in` still works after adding Set/Dict dispatch
// (regression guard — the previous `in` paths must remain intact).
BEZEL_TEST(collections_r9_524, range_in_operator_still_works_control) {
    // Hit: 5 is in 1..10 (inclusive).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 5 in 1..10 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
    // Miss: 11 is not in 1..10 (inclusive).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 11 in 1..10 then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// CONTROL §17.5: char `in` string still works.
BEZEL_TEST(collections_r9_524, char_in_string_still_works_control) {
    // 'e' is in "hello".
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 'e' in \"hello\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
    // 'z' is not in "hello".
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 'z' in \"hello\" then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §17.5: Set<string> — `in` with string elements.
BEZEL_TEST(collections_r9_524, set_string_in_operator_spec_17_5) {
    // Present string element.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<string>(); "
        "  s.Add(\"hello\"); s.Add(\"world\"); "
        "  if \"hello\" in s then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
    // Absent string element.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<string>(); "
        "  s.Add(\"hello\"); s.Add(\"world\"); "
        "  if \"bye\" in s then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// #725 / §4.11: Stack<T>.TryPop(out item): bool — true + writes the top + pops
// on non-empty, false + no-op on empty. (Previously unimplemented: the bool
// result IR-failed in an `if` and the out param was never written.)
BEZEL_TEST(collections_r9, stack_trypop_725) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(42); s.Push(99); "
        "  var popped: int? := null; "  // v0.171 (#1336): TryPop out is T?
        "  var ok := s.TryPop(out popped); "        // ok=true, popped=99
        "  var r := 0; "
        "  if ok then r := r + popped!; end; "      // +99
        "  using var e := new Stack<int>(); "
        "  var x: int? := null; "
        "  if e.TryPop(out x) then r := r + 1000; end; "  // empty → false
        "  return r + s.Count; "                    // 99 + 1 = 100
        "end Main; end C;"), 100);
}

// #725 / §4.12: Queue<T>.TryDequeue(out item): bool — dequeues the FRONT.
BEZEL_TEST(collections_r9, queue_trydequeue_725) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(5); q.Enqueue(7); "
        "  var item: int? := null; "  // v0.171 (#1336): TryDequeue out is T?
        "  var ok := q.TryDequeue(out item); "      // front=5
        "  return (if (ok and item = 5 and q.Count = 1) then 1 else 0); "
        "end Main; end C;"), 1);
}

// #725: TryPeek reads the top (Stack) / front (Queue) WITHOUT removing.
BEZEL_TEST(collections_r9, stack_queue_trypeek_no_removal_725) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<int>(); s.Push(10); s.Push(20); "
        "  var top: int? := null; var ok := s.TryPeek(out top); "  // top=20, kept
        "  using var q := new Queue<int>(); q.Enqueue(3); q.Enqueue(4); "
        "  var front: int? := null; var ok2 := q.TryPeek(out front); "  // front=3, kept
        "  return (if (top = 20 and s.Count = 2 and front = 3 and q.Count = 2) then 1 else 0); "
        "end Main; end C;"), 1);
}
