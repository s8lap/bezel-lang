// Fix #556 — `d.Keys.ToArray()`, `d.Values.ToArray()`, `d.Keys.Count`, and
// `d.Values.Count` crash or miscompile on Dictionary<K,V>.
//
// Spec: stdlib §4.8 (Dictionary<K,V>).  `Keys` returns an ICollection<K> view
// and `Values` returns an ICollection<V> view; both views expose `.Count` (from
// IReadOnlyCollection<T>) and are IEnumerable<T> (so `.ToArray()` / LINQ
// operators from §5 are available via the IEnumerable<T> extension).
//
// Two symptoms, one root cause:
//   * `d.Keys.ToArray()` / `d.Values.ToArray()` → SIGSEGV.
//   * `d.Keys.Count` / `d.Values.Count` → garbage integer (pointer read as i32).
//
// Root cause: the synthetic sema TypeRef produced for `d.Keys` and `d.Values`
// (ExprTypecheck_Calls.cpp ~L3027) had `astNode = nullptr`.  Two guards that
// must both pass relied on `astNode->name->parts[0]`:
//   (a) `isCollectionTypeName()` in LlvmBackend_Calls.cpp:304 — returned false
//       because astNode was null; `.ToArray()` fell through to vtable dispatch
//       on a raw bz_array_t* → SIGSEGV.
//   (b) `isBuiltinColl` in sema at L3046 — same guard; `.Count` was not typed
//       `int`, so codegen read the array struct's first word as i32 → garbage.
//
// Also: `d.Values` as an expression (outside a for-loop) had no lowerExpr
// handler, so `d.Values.ToArray()` and `d.Values.Count` also crashed because
// `lowerExpr(*ma->target)` returned nullptr and the result was used as a
// bz_array_t* / passed to vtable dispatch.
//
// Fix:
//   1. Sema (ExprTypecheck_Calls.cpp): give the synthetic Keys/Values TypeRef
//      a proper `astNode` with name "Array" (mirroring the static kArrayAstNode
//      pattern used for ToDictionary/ToArray results).  This makes
//      `isCollectionTypeName` return true and `isBuiltinColl` pass — Count is
//      typed `int`, and ToArray/LINQ route through the array-backed path.
//   2. Codegen (LlvmBackend_Exprs.cpp): add a `lowerExpr` case for
//      `<dict>.Values` that snapshots and decodes values into a fresh
//      bz_array_t* (mirrors the existing `<dict>.Keys` expression handler).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// d.Keys.Count — §4.8 / §5 / #556
// ---------------------------------------------------------------------------

// #556 / stdlib §4.8: d.Keys.Count must equal the number of entries in the
// dictionary.  Before the fix, astNode was null → isBuiltinColl failed → Count
// was not typed int and codegen read the array struct's first word as garbage.
BEZEL_TEST(dict_keys_values_view_556, keys_count_string_keyed) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"a\"] := 10; "
        "  d[\"b\"] := 20; "
        "  d[\"c\"] := 30; "
        "  return d.Keys.Count; "
        "end Main; end C;"), 3);
}

// #556 / stdlib §4.8: Keys.Count on an empty dictionary is 0.
BEZEL_TEST(dict_keys_values_view_556, keys_count_empty_dict) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  return d.Keys.Count; "
        "end Main; end C;"), 0);
}

// #556 / stdlib §4.8: Keys.Count on a single-entry dictionary is 1.
BEZEL_TEST(dict_keys_values_view_556, keys_count_one_entry) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"only\"] := 42; "
        "  return d.Keys.Count; "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// d.Values.Count — §4.8 / §5 / #556
// ---------------------------------------------------------------------------

// #556 / stdlib §4.8: d.Values.Count must equal the number of entries.
// Before the fix, d.Values as an expression had no lowerExpr handler → nullptr
// passed to bz_array_len → SIGSEGV.
BEZEL_TEST(dict_keys_values_view_556, values_count_string_keyed) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"x\"] := 1; "
        "  d[\"y\"] := 2; "
        "  return d.Values.Count; "
        "end Main; end C;"), 2);
}

// #556 / stdlib §4.8: Values.Count on an empty dictionary is 0.
BEZEL_TEST(dict_keys_values_view_556, values_count_empty_dict) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  return d.Values.Count; "
        "end Main; end C;"), 0);
}

// #556 / stdlib §4.8: Values.Count matches d.Count (same number of entries).
BEZEL_TEST(dict_keys_values_view_556, values_count_matches_dict_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"a\"] := 100; "
        "  d[\"b\"] := 200; "
        "  d[\"c\"] := 300; "
        "  return d.Values.Count; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// d.Keys.ToArray() — §4.8 / §5 / #556
// ---------------------------------------------------------------------------

// #556 / stdlib §4.8 / §5: d.Keys.ToArray() must return an Array<K> containing
// all keys.  Before the fix, isCollectionTypeName returned false for the Keys
// TypeRef → tryLowerLazyLinqCall returned nullptr → vtable dispatch on
// bz_array_t* → SIGSEGV.  Verify by counting the resulting array's length.
BEZEL_TEST(dict_keys_values_view_556, keys_to_array_count_string_keyed) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"p\"] := 1; "
        "  d[\"q\"] := 2; "
        "  using var arr := d.Keys.ToArray(); "
        "  return arr.Count; "
        "end Main; end C;"), 2);
}

// #556 / stdlib §4.8 / §5: d.Keys.ToArray() on an empty dict returns an empty
// Array<K> (Count == 0, not a crash).
BEZEL_TEST(dict_keys_values_view_556, keys_to_array_empty_dict) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  using var arr := d.Keys.ToArray(); "
        "  return arr.Count; "
        "end Main; end C;"), 0);
}

// #556 / stdlib §4.8 / §5: d.Keys.ToArray() on a three-entry dict gives
// an Array<string> with exactly three elements.
BEZEL_TEST(dict_keys_values_view_556, keys_to_array_three_entries) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"one\"] := 1; "
        "  d[\"two\"] := 2; "
        "  d[\"three\"] := 3; "
        "  using var arr := d.Keys.ToArray(); "
        "  return arr.Count; "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// d.Values.ToArray() — §4.8 / §5 / #556
// ---------------------------------------------------------------------------

// #556 / stdlib §4.8 / §5: d.Values.ToArray() must return an Array<V>
// containing all values.  Count the resulting array.
BEZEL_TEST(dict_keys_values_view_556, values_to_array_count_string_keyed) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"a\"] := 10; "
        "  d[\"b\"] := 20; "
        "  using var arr := d.Values.ToArray(); "
        "  return arr.Count; "
        "end Main; end C;"), 2);
}

// #556 / stdlib §4.8 / §5: d.Values.ToArray() on an empty dict gives
// an empty array (Count == 0).
BEZEL_TEST(dict_keys_values_view_556, values_to_array_empty_dict) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  using var arr := d.Values.ToArray(); "
        "  return arr.Count; "
        "end Main; end C;"), 0);
}

// #556 / stdlib §4.8 / §5: d.Values.ToArray() on a three-entry dict produces
// an Array<int> with 3 elements; sum them to verify values are correct.
BEZEL_TEST(dict_keys_values_view_556, values_to_array_int_values_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"a\"] := 5; "
        "  d[\"b\"] := 10; "
        "  d[\"c\"] := 15; "
        "  using var arr := d.Values.ToArray(); "
        "  var sum := 0; "
        "  for var v in arr do sum := sum + v; end; "
        "  return sum; "
        "end Main; end C;"), 30);
}

// ---------------------------------------------------------------------------
// LINQ on d.Keys — §5 / #556
// ---------------------------------------------------------------------------

// #556 / stdlib §5: d.Keys.ToArray() routes through tryLowerLazyLinqCall →
// lowerToLazyEnumerator → bz_lazy_from_array → bz_lazy_to_array (the
// array-backed lazy path).  Verify that a LINQ operator (Take) chained on
// the materialized Keys array also works — confirming the sema fix gives
// the Keys TypeRef a name that isCollectionTypeName recognizes, enabling
// downstream LINQ ops on the array result.
BEZEL_TEST(dict_keys_values_view_556, keys_to_array_linq_take) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"alpha\"] := 1; "
        "  d[\"beta\"] := 2; "
        "  d[\"gamma\"] := 3; "
        "  using var arr := d.Keys.ToArray(); "
        "  using var first2 := arr.Take(2).ToArray(); "
        "  return first2.Count; "
        "end Main; end C;"), 2);
}

// ---------------------------------------------------------------------------
// Regression: existing for-in-Keys / for-in-Values still work — §4.8 / #537
// ---------------------------------------------------------------------------

// #556 regression guard / stdlib §4.8: `for var k in d.Keys` still works after
// the sema TypeRef fix (the for-loop path reads the dict's AST shape directly,
// but let's confirm it hasn't been disturbed).  Sum key lengths: "x"(1) +
// "y"(1) = 2.
BEZEL_TEST(dict_keys_values_view_556, regression_for_in_keys) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"x\"] := 10; "
        "  d[\"y\"] := 20; "
        "  var sum := 0; "
        "  for var k in d.Keys do sum := sum + k.Length; end; "
        "  return sum; "
        "end Main; end C;"), 2);
}

// #556 regression guard / stdlib §4.8: `for var v in d.Values` (fix #537)
// still works after the sema TypeRef and lowerExpr changes.
BEZEL_TEST(dict_keys_values_view_556, regression_for_in_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"a\"] := 7; "
        "  d[\"b\"] := 3; "
        "  var sum := 0; "
        "  for var v in d.Values do sum := sum + v; end; "
        "  return sum; "
        "end Main; end C;"), 10);
}
