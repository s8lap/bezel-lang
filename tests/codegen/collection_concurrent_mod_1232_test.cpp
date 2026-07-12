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
