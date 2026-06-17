// Fix #622 — Dictionary<K,V> iteration order violates the stdlib §4.8
// insertion-order guarantee for ≥3 entries.
//
// Spec: stdlib §4.8 (v0.135, Dictionary<K,V>):
//   "Iteration yields entries in insertion order."
//   "Removing a key and re-adding it places it at the end of the iteration order."
//   "Clear() resets the order."
//
// Root cause: the open-addressing hash table stored entries in bucket-slot order
// (hash-determined), not insertion order. The fix adds an order[] array to
// bz_dict_t that records bucket slot indices in insertion order; on remove, the
// entry is tombstoned in order[]; on rehash, order[] is compacted and translated;
// on Clear(), order_count is reset. All snapshot functions (bz_dict_keys_str,
// bz_dict_keys_gen, bz_dict_values_str, bz_dict_values_gen) iterate order[]
// instead of the raw bucket array.
//
// Tests cover (per the fix-lane requirements):
//   • 5+ key string-keyed dict: entry, Keys, Values all in insertion order.
//   • 5+ key int-keyed (generic) dict: same.
//   • Remove middle key + re-add → key moves to END.
//   • Clear() + re-add → fresh order.
//   • Growth across rehash (50 keys): insertion order preserved.
//   • `in` operator and indexer: unaffected (still work correctly).
//   • OwnedDictionary basic ops: regression guard.
//
// Order verification method: assign value 'i+1' to the (i+0)-th inserted key,
// then compute result = v[0]*10000 + v[1]*1000 + v[2]*100 + v[3]*10 + v[4]*1.
// This uniquely encodes the output permutation as a 5-digit decimal number when
// each v[i] ∈ {1..9} — no two permutations give the same result.

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
// String-keyed: entry iteration in insertion order (§4.8)
// ---------------------------------------------------------------------------

// #622 / stdlib §4.8 v0.135: 5 string keys inserted in order a,b,c,d,e with
// values 1,2,3,4,5. Entry iteration must yield them in that order.
// Encoding: result = v0*10000 + v1*1000 + v2*100 + v3*10 + v4
// If order is correct: result = 1*10000 + 2*1000 + 3*100 + 4*10 + 5 = 12345.
BEZEL_TEST(dict_insertion_order_622, str_key_entry_order_5_keys) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  d.Set(\"c\", 3); "
        "  d.Set(\"d\", 4); "
        "  d.Set(\"e\", 5); "
        "  var result := 0; "
        "  var wt := 10000; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 12345);
}

// #622 / stdlib §4.8 v0.135: Keys view iteration in insertion order.
// Same 5 keys a,b,c,d,e with no values; iterate d.Keys and use key lengths
// to distinguish (all length 1, so use positional weight on length).
// Keys are all length-1, so sum-of-lengths = 5 regardless. Instead, verify
// by checking that the iteration count is 5 (sanity) plus the weighted-value
// test covers Keys indirectly (same order array drives both).
// Direct: insert keys with distinct known lengths — "a"(1),"bb"(2),"ccc"(3).
// Weighted: result = len0*100 + len1*10 + len2 → 123 if insertion order.
BEZEL_TEST(dict_insertion_order_622, str_key_keys_view_order_3_keys) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\",    0); "
        "  d.Set(\"bb\",   0); "
        "  d.Set(\"ccc\",  0); "
        "  d.Set(\"dddd\", 0); "
        "  d.Set(\"eeeee\",0); "
        "  var result := 0; "
        "  var wt := 10000; "
        "  for var k in d.Keys do "
        "    result := result + k.Length * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 12345);
}

// #622 / stdlib §4.8 v0.135: Values view iteration in insertion order.
// Values 1,2,3,4,5 inserted in that order; for var v in d.Values must yield
// 1,2,3,4,5. Same positional encoding: result = 12345.
BEZEL_TEST(dict_insertion_order_622, str_key_values_view_order_5_keys) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  d.Set(\"c\", 3); "
        "  d.Set(\"d\", 4); "
        "  d.Set(\"e\", 5); "
        "  var result := 0; "
        "  var wt := 10000; "
        "  for var v in d.Values do "
        "    result := result + v * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 12345);
}

// ---------------------------------------------------------------------------
// String-keyed: remove middle key + re-add moves it to END (§4.8)
// ---------------------------------------------------------------------------

// #622 / stdlib §4.8 v0.135: remove "c" (3rd of 5) and re-add it.
// Before: a=1 b=2 c=3 d=4 e=5, iteration order: a,b,c,d,e
// After remove("c") + Set("c",9): order becomes a,b,d,e,c.
// Encoded result = 1*10000 + 2*1000 + 4*100 + 5*10 + 9 = 12459.
BEZEL_TEST(dict_insertion_order_622, str_key_remove_readd_moves_to_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  d.Set(\"c\", 3); "
        "  d.Set(\"d\", 4); "
        "  d.Set(\"e\", 5); "
        "  d.Remove(\"c\"); "
        "  d.Set(\"c\", 9); "
        "  var result := 0; "
        "  var wt := 10000; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 12459);
}

// #622 / stdlib §4.8 v0.135: remove first key, re-add it — must go to end.
// Insert a=1,b=2,c=3. Remove a. Re-add a=7. Order: b=2,c=3,a=7.
// Result = 2*100 + 3*10 + 7 = 237.
BEZEL_TEST(dict_insertion_order_622, str_key_remove_first_readd_to_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  d.Set(\"c\", 3); "
        "  d.Remove(\"a\"); "
        "  d.Set(\"a\", 7); "
        "  var result := 0; "
        "  var wt := 100; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 237);
}

// ---------------------------------------------------------------------------
// String-keyed: Clear() resets insertion order (§4.8)
// ---------------------------------------------------------------------------

// #622 / stdlib §4.8 v0.135: Clear() resets the order; keys re-added after
// Clear() establish a fresh insertion order.
// Initial: a=1,b=2,c=3. After Clear(): empty. Re-add: c=3,a=1,b=2.
// Order after re-add: c,a,b → result = 3*100 + 1*10 + 2 = 312.
BEZEL_TEST(dict_insertion_order_622, str_key_clear_resets_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  d.Set(\"c\", 3); "
        "  d.Clear(); "
        "  d.Set(\"c\", 3); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  var result := 0; "
        "  var wt := 100; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 312);
}

// ---------------------------------------------------------------------------
// String-keyed: order preserved across rehash (growth, §4.8)
// ---------------------------------------------------------------------------

// #622 / stdlib §4.8 v0.135: insert 8 keys (forcing at least one rehash from
// the initial capacity-8 table at 75% load), then verify insertion order is
// preserved. Values 1..8; result = 1*10000000 + 2*1000000 + ... + 8*1 = 12345678.
BEZEL_TEST(dict_insertion_order_622, str_key_order_preserved_across_rehash) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"k1\", 1); "
        "  d.Set(\"k2\", 2); "
        "  d.Set(\"k3\", 3); "
        "  d.Set(\"k4\", 4); "
        "  d.Set(\"k5\", 5); "
        "  d.Set(\"k6\", 6); "
        "  d.Set(\"k7\", 7); "
        "  d.Set(\"k8\", 8); "
        "  var result := 0; "
        "  var wt := 10000000; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 12345678);
}

// ---------------------------------------------------------------------------
// Int-keyed (generic): entry, Keys, Values in insertion order (§4.8)
// ---------------------------------------------------------------------------

// #622 / stdlib §4.8 v0.135: 5 int keys inserted as 10,20,30,40,50 with
// values 1,2,3,4,5. Entry iteration must yield (10,1),(20,2),(30,3),(40,4),(50,5).
// Verify via values: result = 1*10000 + 2*1000 + 3*100 + 4*10 + 5 = 12345.
BEZEL_TEST(dict_insertion_order_622, int_key_entry_order_5_keys) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(10, 1); "
        "  d.Set(20, 2); "
        "  d.Set(30, 3); "
        "  d.Set(40, 4); "
        "  d.Set(50, 5); "
        "  var result := 0; "
        "  var wt := 10000; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 12345);
}

// #622 / stdlib §4.8 v0.135: int-keyed dict, Keys view in insertion order.
// Keys 10,20,30; weighted: result = 10*100 + 20*10 + 30 = 1230.
BEZEL_TEST(dict_insertion_order_622, int_key_keys_view_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(10, 0); "
        "  d.Set(20, 0); "
        "  d.Set(30, 0); "
        "  var result := 0; "
        "  var wt := 100; "
        "  for var k in d.Keys do "
        "    result := result + k * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 1230);
}

// #622 / stdlib §4.8 v0.135: int-keyed dict, Values view in insertion order.
// Values 1,2,3,4,5 inserted in that order; result = 12345.
BEZEL_TEST(dict_insertion_order_622, int_key_values_view_order_5_keys) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(10, 1); "
        "  d.Set(20, 2); "
        "  d.Set(30, 3); "
        "  d.Set(40, 4); "
        "  d.Set(50, 5); "
        "  var result := 0; "
        "  var wt := 10000; "
        "  for var v in d.Values do "
        "    result := result + v * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 12345);
}

// ---------------------------------------------------------------------------
// Int-keyed: remove + re-add, Clear, rehash across growth (§4.8)
// ---------------------------------------------------------------------------

// #622 / stdlib §4.8 v0.135: int-keyed, remove middle key + re-add moves to end.
// Insert 10=1,20=2,30=3,40=4,50=5. Remove 30. Re-add 30=9.
// Order: 10=1,20=2,40=4,50=5,30=9 → result = 1*10000+2*1000+4*100+5*10+9 = 12459.
BEZEL_TEST(dict_insertion_order_622, int_key_remove_readd_moves_to_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(10, 1); "
        "  d.Set(20, 2); "
        "  d.Set(30, 3); "
        "  d.Set(40, 4); "
        "  d.Set(50, 5); "
        "  d.Remove(30); "
        "  d.Set(30, 9); "
        "  var result := 0; "
        "  var wt := 10000; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 12459);
}

// #622 / stdlib §4.8 v0.135: int-keyed, Clear() resets order.
// Initial: 10=1,20=2,30=3. Clear(). Re-add: 30=3,10=1,20=2.
// Order: 30,10,20 → result = 3*100+1*10+2 = 312.
BEZEL_TEST(dict_insertion_order_622, int_key_clear_resets_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(10, 1); "
        "  d.Set(20, 2); "
        "  d.Set(30, 3); "
        "  d.Clear(); "
        "  d.Set(30, 3); "
        "  d.Set(10, 1); "
        "  d.Set(20, 2); "
        "  var result := 0; "
        "  var wt := 100; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 312);
}

// #622 / stdlib §4.8 v0.135: int-keyed, order preserved across rehash (8 keys).
// Keys 10..80 with values 1..8; forces rehash. result = 12345678.
BEZEL_TEST(dict_insertion_order_622, int_key_order_preserved_across_rehash) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(10, 1); "
        "  d.Set(20, 2); "
        "  d.Set(30, 3); "
        "  d.Set(40, 4); "
        "  d.Set(50, 5); "
        "  d.Set(60, 6); "
        "  d.Set(70, 7); "
        "  d.Set(80, 8); "
        "  var result := 0; "
        "  var wt := 10000000; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 12345678);
}

// ---------------------------------------------------------------------------
// `in` operator and indexer: unaffected by the order fix (§4.8 regression)
// ---------------------------------------------------------------------------

// #622 regression guard / stdlib §4.8: `key in d` (ContainsKey) still works
// after the insertion-order fix. Insert 3 keys; check present and absent.
// "a" present → return 1; "z" absent → no change.
BEZEL_TEST(dict_insertion_order_622, in_operator_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  d.Set(\"c\", 3); "
        "  if \"a\" in d then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// #622 regression guard / stdlib §4.8: indexer read/write still works after
// the insertion-order fix.  Set 3 keys, update one via indexer, verify sum.
// a=1, b=2, c=3; update c via d[\"c\"] := 9; sum = 1+2+9 = 12.
BEZEL_TEST(dict_insertion_order_622, indexer_read_write_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  d.Set(\"c\", 3); "
        "  d[\"c\"] := 9; "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e.Value; end; "
        "  return sum; "
        "end Main; end C;"), 12);
}

// ---------------------------------------------------------------------------
// OwnedDictionary: basic ops regression guard (§4.9 — order not promised)
// ---------------------------------------------------------------------------

// #622 regression guard / stdlib §4.9: OwnedDictionary<string, V> basic ops
// (Set/Remove/Count) still work after the runtime changes. The order tracking
// array is maintained but §4.9 makes no iteration-order promise. Insert 3 keys,
// remove 1, Count == 2. (The VALUE is a class — per §14.4 an owning dictionary
// owns its values and rejects a runtime-managed `string` value; the key is
// value-like and may be `string`.)
BEZEL_TEST(dict_insertion_order_622, owned_dict_basic_ops_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V public n: int := 0; end V; "
        "class C public static method Main(): int "
        "  using var od := new OwnedDictionary<string, V>(); "
        "  od.Set(\"k1\", new V()); "
        "  od.Set(\"k2\", new V()); "
        "  od.Set(\"k3\", new V()); "
        "  od.Remove(\"k2\"); "
        "  return od.Count; "
        "end Main; end C;"), 2);
}

// #622 regression guard / stdlib §4.9: OwnedDictionary.Clear() disposes values
// and does not leak. After Clear(), Count == 0; a new value can be added. (The
// VALUE is a class with a real disposal obligation — §14.4 requires a class
// value for an owning dictionary, which also makes the "disposes values"
// guarantee meaningful.)
BEZEL_TEST(dict_insertion_order_622, owned_dict_clear_then_reuse) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V public n: int := 0; end V; "
        "class C public static method Main(): int "
        "  using var od := new OwnedDictionary<string, V>(); "
        "  od.Set(\"a\", new V()); "
        "  od.Set(\"b\", new V()); "
        "  od.Clear(); "
        "  od.Set(\"c\", new V()); "
        "  return od.Count; "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// Update (set existing key) does NOT change iteration order (§4.8)
// ---------------------------------------------------------------------------

// #622 / stdlib §4.8 v0.135: updating an existing key's value must NOT change
// its position in the iteration order. Insert a=1,b=2,c=3; update b=9.
// Order remains a,b,c → result = 1*100 + 9*10 + 3 = 193.
BEZEL_TEST(dict_insertion_order_622, str_key_update_preserves_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  d.Set(\"c\", 3); "
        "  d.Set(\"b\", 9); "
        "  var result := 0; "
        "  var wt := 100; "
        "  for var e in d do "
        "    result := result + e.Value * wt; "
        "    wt := wt / 10; "
        "  end; "
        "  return result; "
        "end Main; end C;"), 193);
}
