// Fix #1232 — §29.12: modifying a collection during for-in iteration must
// throw InvalidOperationException on the next MoveNext().
//
// Spec §29.12 (v0.154):
//   "Modifying a collection while iterating it invalidates the active
//    enumerator. Standard collections detect this and throw
//    InvalidOperationException on the next MoveNext()."
//
// Implementation: each collection struct carries a mod_count (int64 epoch)
// bumped on every structural mutation (Add/Remove/Insert/Clear/etc.).
// lowerArrayFor / emitGenericKeyTableFor / lowerDictKeysFor /
// lowerDictValuesFor / lowerDictEntryFor / lowerLinkedListFor each:
//   1. Capture the epoch before the loop starts.
//   2. On every condBB visit (= MoveNext), compare current mod_count to the
//      captured epoch; mismatch → emitGuardThrow("InvalidOperationException").
//
// Test groups:
//   A. Throw on mutation during iteration (one test per collection type).
//   B. Normal iteration (no mutation) must NOT throw — no false positives.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
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
// Part A: mutation during iteration → InvalidOperationException
// ===========================================================================

// §29.12 / #1232 — Array<int>: Add() during for-in throws.
BEZEL_TEST(collection_concurrent_mod_1232, array_add_during_for_throws) {
    // Add() is a structural mutation (bumps mod_count) → exception on next MoveNext.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); xs.Add(1); xs.Add(2); xs.Add(3); "
        "  try "
        "    for var x in xs do xs.Add(99); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #1232 — Array<int>: Remove() during for-in throws.
BEZEL_TEST(collection_concurrent_mod_1232, array_remove_during_for_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); xs.Add(10); xs.Add(20); xs.Add(30); "
        "  try "
        "    for var x in xs do xs.Remove(10); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #1232 — Dictionary<string,int>: set new key during for-in throws.
BEZEL_TEST(collection_concurrent_mod_1232, dict_add_during_keys_for_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string,int>(); "
        "  d[\"a\"] := 1; d[\"b\"] := 2; "
        "  try "
        "    for var k in d.Keys do d[\"c\"] := 3; end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #1232 — Dictionary<string,int>: remove key during for-in throws.
BEZEL_TEST(collection_concurrent_mod_1232, dict_remove_during_values_for_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string,int>(); "
        "  d[\"x\"] := 10; d[\"y\"] := 20; "
        "  try "
        "    for var v in d.Values do d.Remove(\"x\"); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #1232 — Dictionary<string,int>: add key during entry for-in throws.
BEZEL_TEST(collection_concurrent_mod_1232, dict_add_during_entry_for_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string,int>(); "
        "  d[\"p\"] := 1; d[\"q\"] := 2; "
        "  try "
        "    for var e in d do d[\"r\"] := 3; end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #1232 — Set<string>: add during for-in throws.
BEZEL_TEST(collection_concurrent_mod_1232, set_add_during_for_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := new Set<string>(); "
        "  s.Add(\"a\"); s.Add(\"b\"); "
        "  try "
        "    for var item in s do s.Add(\"c\"); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #1232 — LinkedList<int>: AddLast during for-in throws.
BEZEL_TEST(collection_concurrent_mod_1232, linkedlist_add_during_for_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var ll := new LinkedList<int>(); "
        "  ll.AddLast(1); ll.AddLast(2); "
        "  try "
        "    for var n in ll do ll.AddLast(99); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Part B: normal iteration (no mutation) — must NOT throw (no false positives)
// ===========================================================================

// §29.12 / #1232 — Array normal for-in: reading elements must not throw.
BEZEL_TEST(collection_concurrent_mod_1232, array_normal_iteration_no_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); xs.Add(1); xs.Add(2); xs.Add(3); "
        "  var s := 0; "
        "  for var x in xs do s := s + x; end; "
        "  return s; "
        "end Main; end C;"), 6);
}

// §29.12 / #1232 — Dictionary.Keys normal for-in: no throw.
BEZEL_TEST(collection_concurrent_mod_1232, dict_keys_normal_iteration_no_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string,int>(); "
        "  d[\"a\"] := 10; d[\"b\"] := 20; d[\"c\"] := 30; "
        "  var s := 0; "
        "  for var k in d.Keys do s := s + d[k]; end; "
        "  return s; "
        "end Main; end C;"), 60);
}

// §29.12 / #1232 — Dictionary.Values normal for-in: no throw.
BEZEL_TEST(collection_concurrent_mod_1232, dict_values_normal_iteration_no_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string,int>(); "
        "  d[\"x\"] := 5; d[\"y\"] := 7; "
        "  var s := 0; "
        "  for var v in d.Values do s := s + v; end; "
        "  return s; "
        "end Main; end C;"), 12);
}

// §29.12 / #1232 — Dictionary entry normal for-in: no throw.
BEZEL_TEST(collection_concurrent_mod_1232, dict_entry_normal_iteration_no_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string,int>(); "
        "  d[\"p\"] := 3; d[\"q\"] := 4; "
        "  var s := 0; "
        "  for var e in d do s := s + e.Value; end; "
        "  return s; "
        "end Main; end C;"), 7);
}

// §29.12 / #1232 — LinkedList normal for-in: no throw.
BEZEL_TEST(collection_concurrent_mod_1232, linkedlist_normal_iteration_no_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var ll := new LinkedList<int>(); "
        "  ll.AddLast(10); ll.AddLast(20); ll.AddLast(30); "
        "  var s := 0; "
        "  for var n in ll do s := s + n; end; "
        "  return s; "
        "end Main; end C;"), 60);
}

// §29.12 / #1232 — Set normal for-in: no throw.
BEZEL_TEST(collection_concurrent_mod_1232, set_normal_iteration_no_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := new Set<int>(); "
        "  s.Add(1); s.Add(2); s.Add(3); "
        "  var count := 0; "
        "  for var item in s do count := count + 1; end; "
        "  return count; "
        "end Main; end C;"), 3);
}

// §29.12 / #1232 — Array: indexer-set on EXISTING element is not structural →
// must NOT throw (only size-changing ops bump mod_count).
BEZEL_TEST(collection_concurrent_mod_1232, array_indexer_set_no_throw) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); xs.Add(1); xs.Add(2); xs.Add(3); "
        "  var s := 0; "
        "  for var x in xs do xs[0] := 99; s := s + x; end; "
        "  return s; "
        "end Main; end C;"), 6);
}

// ===========================================================================
// fix #4606 / §29.12, v0.307 — a dictionary indexer-set on a PRESENT key is
// an element-level write, not a structural one: "`dict[existingKey] := v`
// updating a present key ... leaves the enumerator valid." Pre-fix,
// bz_dict_set_str / bz_dict_set_str_disposing / bz_dict_set_gen /
// bz_dict_set_gen_disposing all bumped mod_count on the present-key branch
// ("value update = modification"), so a `for` over the dictionary that
// updates a present key's value falsely raised InvalidOperationException at
// the next MoveNext(). These three rows each pin a distinct one of the four
// runtime set entry points (string-key / generic-key / disposing).
// ===========================================================================

// bz_dict_set_str: string-key Dictionary, entry ("for var e in d") iteration
// — the exact shape #4606 was filed against. Pre-fix: uncaught
// InvalidOperationException (never reaches the return). Post-fix: completes
// the pass and returns the sum of the (snapshot) values, 30 + 25 = 55 — the
// runtime's dict entry-for loop reads a key/value snapshot taken at loop
// entry (LlvmBackend_Stmts.cpp's lowerDictEntryFor), so the in-loop update is
// exercised here for its non-invalidation only, matching the rule-corpus
// case s29_12_0010.
BEZEL_TEST(collection_concurrent_mod_1232,
           dict_str_key_indexer_set_existing_key_no_throw_4606) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string,int>(); "
        "  d[\"alice\"] := 30; d[\"bob\"] := 25; "
        "  var total := 0; "
        "  for var e in d do "
        "    d[e.Key] := e.Value + 1; "
        "    total := total + e.Value; "
        "  end; "
        "  return total; "
        "end Main; end C;"), 55);
}

// bz_dict_set_gen: generic-key (int) Dictionary, entry iteration. Same shape
// as above, routed through the generic-key runtime instead of the
// string-key one. Pre-fix: uncaught InvalidOperationException.
BEZEL_TEST(collection_concurrent_mod_1232,
           dict_gen_key_indexer_set_existing_key_no_throw_4606) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<int,int>(); "
        "  d[1] := 10; d[2] := 20; "
        "  var s := 0; "
        "  for var e in d do "
        "    d[e.Key] := e.Value + 1; "
        "    s := s + e.Value; "
        "  end; "
        "  return s; "
        "end Main; end C;"), 30);
}

// bz_dict_set_str_disposing: OwningDictionary<string, R>'s owned-taking
// indexer-set on a PRESENT key. §4.9's default entry-for is not provided on
// OwningDictionary, so iteration uses the key-iteration pattern (`.Keys`,
// which shares the same dict's mod_count epoch). Pre-fix: the update on
// "bob" (not yet visited) bumped mod_count, and the loop's next advance threw
// InvalidOperationException, which the catch converts to `caught := 1` — so
// the pre-fix result is 100 + R._n. Post-fix: no throw (caught stays 0), and
// the disposing indexer-set disposes the value it displaced under "bob"
// exactly once (R._n == 1) — so the expected result is 1.
BEZEL_TEST(collection_concurrent_mod_1232,
           owning_dict_indexer_set_existing_key_no_throw_4606) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"a\", move (new R())); "
        "  d.Add(\"b\", move (new R())); "
        "  var caught := 0; "
        "  try "
        "    for var k in d.Keys do "
        "      if k = \"a\" then d[\"b\"] := move (new R()); end; "
        "    end; "
        "  catch InvalidOperationException => caught := 1; "
        "  end; "
        "  return caught * 100 + R._n; "
        "end Main; end C;"), 1);
}

// bz_dict_set_gen_disposing: OwningDictionary<int, R>'s owned-taking
// indexer-set on a PRESENT key — the GENERIC-key twin of the string-key row
// above (#4606's fix touched four runtime entry points but the commit shipped
// only three rows; this is the missing fourth). Reachable from real code:
// LlvmBackend_Assign.cpp's lowerCollectionIndexerSet takes the `keyTy`
// (non-null / non-string) branch for any non-string-keyed owning dictionary,
// which falls into the `dictIsOwning2 && valDisp` arm and calls
// bz_dict_set_gen_disposing — stdlib §4.9 documents `OwningDictionary<K, V>`
// keys as "any type with the standard equality contract" ("typically string,
// int, or a record"), so `OwningDictionary<int, R>` is ordinary, sanctioned
// usage, not a corner case. Same shape as the string-key row: §4.9's default
// entry-for is not provided on OwningDictionary, so iteration uses the
// key-iteration pattern (`.Keys`). Pre-fix (bz_dict_set_gen_disposing bumping
// mod_count on the present-key branch): the update on key 2 (not yet
// visited) bumps mod_count, and the loop's next advance throws
// InvalidOperationException, converted by the catch to `caught := 1` — result
// 100 + R._n. Post-fix: no throw (caught stays 0), and the disposing
// indexer-set disposes the value it displaced under key 2 exactly once
// (R._n == 1) — expected result 1.
BEZEL_TEST(collection_concurrent_mod_1232,
           owning_dict_gen_key_indexer_set_existing_key_no_throw_4606) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<int, R>(); "
        "  d.Add(1, move (new R())); "
        "  d.Add(2, move (new R())); "
        "  var caught := 0; "
        "  try "
        "    for var k in d.Keys do "
        "      if k = 1 then d[2] := move (new R()); end; "
        "    end; "
        "  catch InvalidOperationException => caught := 1; "
        "  end; "
        "  return caught * 100 + R._n; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// fix #4623 / §29.12, v0.307 — Set<string>.Remove()/.Clear() never bump
// mod_count at all: unlike every sibling structural-mutation path in this
// file (Array Add/Remove/InsertAt/RemoveAt, Dictionary set/remove/pop/clear,
// LinkedList Add*/Remove*/Pop*, generic-key Set<T> via bz_dict_remove_gen /
// bz_dict_clear), bz_dict_remove_set_string and bz_dict_clear_set_strings —
// the runtime behind Set<string>.Remove()/.Clear() — read/write the table
// but skip d->mod_count++ entirely. This is the FALSE-NEGATIVE direction,
// the opposite of #4606 (which over-reported): a `for` over a Set<string>
// that removes or clears mid-iteration does not raise
// InvalidOperationException where §29.12 requires it to (Remove/Clear are
// listed structural operations; the element-level-write carve-out #4606 was
// about does not apply — there is no element-level write here, both remove
// storage). Confirmed pre-fix: both rows below return 0 (no exception
// caught) instead of the expected 42.
// ===========================================================================

// §29.12 / #4623 — Set<string>: Remove() during for-in must throw.
BEZEL_TEST(collection_concurrent_mod_1232, set_string_remove_during_for_throws_4623) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := new Set<string>(); "
        "  s.Add(\"a\"); s.Add(\"b\"); "
        "  try "
        "    for var item in s do s.Remove(\"a\"); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #4623 — Set<string>: Clear() during for-in must throw.
BEZEL_TEST(collection_concurrent_mod_1232, set_string_clear_during_for_throws_4623) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := new Set<string>(); "
        "  s.Add(\"a\"); s.Add(\"b\"); "
        "  try "
        "    for var item in s do s.Clear(); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// fix #4631 / §29.12 — Array<T>.Sort() / Sort(comparer) never bump mod_count.
// Spec §29.12 lists the in-place Sort()/Reverse() as structural operations
// (they reposition entries), so they must bump mod_count exactly like
// Add/Remove/Insert/Clear do. bz_array_sort and bz_array_sort_closure in
// src/runtime/bezel_runtime_collections.cpp ran qsort but returned without
// arr->mod_count++, so a `for` over an Array<T> that called Sort() (either
// form) inside the loop body did NOT raise InvalidOperationException at the
// next MoveNext() — the FALSE-NEGATIVE direction. These two rows pin each
// form; pre-fix both return 0 (no exception caught).
// ===========================================================================

// §29.12 / #4631 — Array<int>: Sort() during for-in must throw.
BEZEL_TEST(collection_concurrent_mod_1232,
           array_sort_during_for_throws_4631) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); "
        "  xs.Add(3); xs.Add(1); xs.Add(2); "
        "  try "
        "    for var x in xs do xs.Sort(); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #4631 — Array<int>: Sort(comparer) during for-in must throw.
// The comparer-form routes through bz_array_sort_closure (closure-bearing
// shim around qsort); pre-fix the closure path also skipped mod_count++.
BEZEL_TEST(collection_concurrent_mod_1232,
           array_sort_comparer_during_for_throws_4631) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); "
        "  xs.Add(3); xs.Add(1); xs.Add(2); "
        "  try "
        "    for var x in xs do xs.Sort(lambda(a: int, b: int) => a - b); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// fix #4631 (Reverse half) / §29.12, v0.307 — the in-place Array<T>.Reverse()
// never bumped mod_count.
//
// §29.12 names the two in-place reorderings together — "together with the
// in-place `Sort()`/`Reverse()` (they reposition)" — so each must invalidate a
// live enumerator. #4631's fix landed the Sort() half only (bz_array_sort /
// bz_array_sort_closure gained their bump) and the issue was closed; the
// Reverse() half, which that issue itself called "worse", was left undone.
//
// Reverse() is not a runtime call at all: Emitter::tryLowerArrayCall lowers it
// as an INLINED IR element-swap loop (LlvmBackend_Calls.cpp, the "rev.c" /
// "rev.b" / "rev.e" blocks) that reaches the runtime only through
// bz_array_ptr_at, so there was no site bumping the epoch. A `for` over an
// Array<T> that reversed it in the loop body therefore kept walking
// repositioned elements and never raised InvalidOperationException — the
// FALSE-NEGATIVE direction, exactly as Sort() did before #4631.
//
// The fix adds bz_array_note_repositioned (bezel_runtime_collections.cpp) and
// calls it after the emitted swap loop. Pre-fix all three throwing rows below
// return 0 (no exception caught).
// ===========================================================================

// §29.12 / #4631 — Array<int>: Reverse() during for-in must throw.
// This is issue #4631's own stated repro.
BEZEL_TEST(collection_concurrent_mod_1232,
           array_reverse_during_for_throws_4631) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); xs.Add(3); "
        "  try "
        "    for var x in xs do xs.Reverse(); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #4631 + #3345 — the STATIC call form `Array.Reverse(xs)` re-binds
// the receiver to args[0] and reaches the same inlined swap loop, so it must
// fault identically. Pinned separately because the two spellings enter the arm
// through different guards.
BEZEL_TEST(collection_concurrent_mod_1232,
           array_static_reverse_during_for_throws_4631) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); xs.Add(3); "
        "  try "
        "    for var x in xs do Array.Reverse(xs); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #4631 — the OWNING family counterpart. §29.12's structural list
// covers the standard collections "together with their owning-collection
// counterparts", and OwningArray<T>.Reverse() (stdlib §4.4) lowers through the
// very same arm, so it must fault too.
BEZEL_TEST(collection_concurrent_mod_1232,
           owning_array_reverse_during_for_throws_4631) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Item public property N: int; "
        "  public constructor(n: int) N := n; end constructor; end Item; "
        "class C public static method Main(): int "
        "  using var xs := new OwningArray<Item>(); "
        "  xs.Add(new Item(1)); xs.Add(new Item(2)); xs.Add(new Item(3)); "
        "  try "
        "    for var it in xs do xs.Reverse(); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// §29.12 / #4631 — NO FALSE POSITIVE: a Reverse() that cannot have
// repositioned anything invalidates nothing. A one-element array's swap loop
// runs zero iterations, so bz_array_note_repositioned's `count > 1` guard (the
// same guard bz_array_sort carries) must leave the enumerator valid and the
// loop must complete normally. Returns the single element, 7.
BEZEL_TEST(collection_concurrent_mod_1232,
           array_reverse_singleton_during_for_no_throw_4631) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); xs.Add(7); "
        "  var s := 0; "
        "  for var x in xs do xs.Reverse(); s := s + x; end; "
        "  return s; "
        "end Main; end C;"), 7);
}

// §29.12 / #4631 — Reverse() STILL REVERSES. The fix appends a call after the
// emitted swap loop; this row pins that the loop itself is untouched, so a
// Reverse() outside any iteration keeps reordering the elements. [1,2,3,4]
// reversed is [4,3,2,1]; 4*10 + 1 = 41 reads both ends.
BEZEL_TEST(collection_concurrent_mod_1232,
           array_reverse_outside_loop_still_reorders_4631) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); xs.Add(3); xs.Add(4); "
        "  xs.Reverse(); "
        "  return xs[0] * 10 + xs[3]; "
        "end Main; end C;"), 41);
}
