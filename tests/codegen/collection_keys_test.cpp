// Codegen — non-string, non-record Dictionary keys / Set elements, and record
// Dictionary VALUES (stdlib §4.8 Dictionary<K,V> / §4.10 Set<T>).
//
// The record-as-key work (record_key_test.cpp) wired the generic-key runtime
// path for STRING keys (fast path) and RECORD keys (trampolines). These tests
// cover the previously-broken NON-string/NON-record cases:
//
//   D3 — primitive/enum-keyed Dictionary. §4.8: "K must provide consistent
//        Equals and GetHashCode"; int and enum qualify. Such keys route through
//        the generic-key runtime (bz_dict_*_gen) with hash/eq trampolines built
//        over the key's value hash (§8.3) / integer compare — NOT the string
//        fast path (which reinterpreted the integer as a pointer and crashed).
//   D4 — record-typed Dictionary VALUE. V is unconstrained for the borrowing
//        Dictionary (§4.8), so a record is a valid value. A record value is an
//        aggregate; it is heap-boxed on Set and copied back on TryGetValue.
//   D8 — Set<int> / Set<string> deduplication. §4.10: a Set deduplicates by the
//        element's equality contract; `Set<int> := [1,2,2,3]` yields {1,2,3}.
//        Non-record element sets are dict-backed (the array backing could not
//        deduplicate) so Add is idempotent by content.
//
// Each of these crashed (D3 segfault, D4 SIGILL) or returned a wrong result
// (D8 Count too high) before the fix.

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

// --- D3: primitive-keyed Dictionary (§4.8) ---

// §4.8: Dictionary<int,int> set/get/contains/count round-trip. Re-setting an
// existing int key updates its value (one entry); a distinct key adds another.
// TryGetValue retrieves each value; ContainsKey is true for a present key and
// false for an absent one. (Before the fix the int key hit the string fast path
// — loadStringData on the integer-as-pointer — and segfaulted.)
BEZEL_TEST(codegen_collection_keys, dict_int_key_roundtrip_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d[1] := 100; "
        "  d[2] := 200; "
        "  d[1] := 111; "                       // updates key 1, no new entry
        "  var v: int? := null; d.TryGetValue(1, out v); "  // v0.171 (#1336): out is V?
        "  var w: int? := null; d.TryGetValue(2, out w); "
        "  return int(d.Count = 2 and v = 111 and w = 200 "
        "         and d.ContainsKey(1) and not d.ContainsKey(9)); "
        "end Main; end C;"), 1);
}

// §4.8: ContainsKey on an EMPTY int-keyed Dictionary is false (no crash). This
// is the minimal reproducer that segfaulted before the fix — even an empty dict
// plus ContainsKey took the broken string path.
BEZEL_TEST(codegen_collection_keys, dict_int_key_empty_contains_false_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  return int(d.ContainsKey(1)); "        // empty → false → 0
        "end Main; end C;"), 0);
}

// §4.8: ContainsKey(param) on an int-keyed Dictionary passed by parameter. This
// is the form that aborted the LLVM backend ("Cannot emit physreg copy
// instruction") before the fix; the generic path spills the key to a stack slot
// and passes its address, which the backend lowers cleanly.
BEZEL_TEST(codegen_collection_keys, dict_int_key_containskey_param_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Lookup(d: Dictionary<int, int>, k: int): bool "
        "    return d.ContainsKey(k); "
        "  end Lookup; "
        "  public static method Main(): int "
        "    using var d := new Dictionary<int, int>(); "
        "    d[7] := 70; "
        "    return int(Lookup(d, 7) and not Lookup(d, 8)); "
        "  end Main; "
        "end C;"), 1);
}

// §8.3 hash-equality invariant: an int key built by arithmetic (a distinct value
// computation) lands in the SAME entry as the literal key, so the second Set
// updates rather than adds. Count stays 1; the value is the updated one.
BEZEL_TEST(codegen_collection_keys, dict_int_key_equal_keys_same_entry_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d[5] := 100; "
        "  d[2 + 3] := 999; "                   // equal key, distinct build
        "  var v: int? := null; d.TryGetValue(5, out v); "  // v0.171 (#1336): out is V?
        "  return int(d.Count = 1 and v = 999); "
        "end Main; end C;"), 1);
}

// --- D3: enum-keyed Dictionary (§4.8) ---

// §4.8: Dictionary<enum,int> round-trip. An enum value qualifies as a key (it
// has value equality and a value-based hash, §8.3); it lowers to an integer and
// routes through the generic path with an integer compare.
BEZEL_TEST(codegen_collection_keys, dict_enum_key_roundtrip_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Color, int>(); "
        "  d[Color.Red] := 50; "
        "  d[Color.Blue] := 70; "
        "  var v: int? := null; d.TryGetValue(Color.Blue, out v); "  // v0.171 (#1336): out is V?
        "  return int(d.Count = 2 and v = 70 "
        "         and d.ContainsKey(Color.Red) "
        "         and not d.ContainsKey(Color.Green)); "
        "end Main; end C;"), 1);
}

// --- D4: record-typed Dictionary VALUE (§4.8) ---

// §4.8: Dictionary<string, Point> Set then TryGetValue copies the record value
// back out. V is unconstrained, so a record value is valid; it is heap-boxed on
// insertion and copied into the out slot on retrieval. (Before the fix, Set of a
// record value crashed the compiler with SIGILL — the aggregate value hit the
// scalar inttoptr value-encoding path.)
BEZEL_TEST(codegen_collection_keys, dict_record_value_set_get_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d[\"a\"] := new Point(1, 2); "
        "  d[\"b\"] := new Point(3, 4); "
        "  var p: Point? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(\"a\", out p); "
        "  var q: Point? := null; "
        "  d.TryGetValue(\"b\", out q); "
        "  return int(d.Count = 2 and p!.x = 1 and p!.y = 2 "
        "         and q!.x = 3 and q!.y = 4); "
        "end Main; end C;"), 1);
}

// §4.8: a record value's fields are independent per key (distinct boxes). Updating
// a key's record value replaces it; the other key's value is untouched.
BEZEL_TEST(codegen_collection_keys, dict_record_value_update_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d[\"a\"] := new Point(1, 2); "
        "  d[\"b\"] := new Point(3, 4); "
        "  d[\"a\"] := new Point(9, 9); "       // update a's record value
        "  var p: Point? := null; d.TryGetValue(\"a\", out p); "  // v0.171 (#1336): out is V?
        "  var q: Point? := null; d.TryGetValue(\"b\", out q); "
        "  return int(d.Count = 2 and p!.x = 9 and p!.y = 9 "
        "         and q!.x = 3 and q!.y = 4); "
        "end Main; end C;"), 1);
}

// --- D8: Set<int> / Set<string> deduplication (§4.10) ---

// §4.10: Set<int> deduplicates — Add(5); Add(7); Add(5) yields Count 2, not 3.
// (Before the fix, Set<int> was array-backed and Add appended unconditionally,
// giving Count 3.) Membership reflects the two distinct values.
BEZEL_TEST(codegen_collection_keys, set_int_dedup_count_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(5); s.Add(7); s.Add(5); "
        "  return int(s.Count = 2 and s.Contains(5) and s.Contains(7) "
        "         and not s.Contains(9)); "
        "end Main; end C;"), 1);
}

// §4.10: Set<string> deduplicates by string content equality (§4.6). A duplicate
// string built by concatenation (a distinct heap allocation) collapses to the
// same entry; Count reflects unique contents.
BEZEL_TEST(codegen_collection_keys, set_string_dedup_count_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Set<string>(); "
        "  s.Add(\"alpha\"); "
        "  s.Add(\"beta\"); "
        "  s.Add(\"al\" + \"pha\"); "             // equal content, distinct heap
        "  return int(s.Count = 2 and s.Contains(\"alpha\") "
        "         and s.Contains(\"beta\") and not s.Contains(\"gamma\")); "
        "end Main; end C;"), 1);
}

// §4.10 / §4.6: Set<int> Remove drops a present element by value equality; Count
// falls and Contains becomes false.
BEZEL_TEST(codegen_collection_keys, set_int_remove_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(1); s.Add(2); s.Add(3); "
        "  var removed := s.Remove(2); "
        "  return int(removed and s.Count = 2 "
        "         and not s.Contains(2) and s.Contains(1) and s.Contains(3)); "
        "end Main; end C;"), 1);
}

// --- #370: iteration over a dict-backed Set<T> (§4.10) ---
//
// A Set<T> is `ICollection<T>` (§4.10), so `for var x in set` yields the
// elements. The set is now generic-hash-table-backed (D8), so the array-backed
// for-loop read garbage; iteration must enumerate the table's keys. The loop
// variable's element type comes from the set's `T` (sema types the set), so the
// element value is usable in the body.

// §4.10: `for var x in <Set<int>>` sums the DEDUPED elements. Add(5), Add(7),
// Add(5) → {5, 7}; the sum is 12 (the duplicate 5 contributes once). Before the
// fix this iterated bz_array_* over the dict bytes and produced garbage.
BEZEL_TEST(codegen_collection_keys, for_in_set_int_sum_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(5); s.Add(7); s.Add(5); "
        "  var sum := 0; "
        "  for var x in s do sum := sum + x; end; "
        "  return sum; "                              // 5 + 7 = 12
        "end Main; end C;"), 12);
}

// §4.10: `for var x in <Set<int>>` runs once per DISTINCT element — a counted
// loop over the deduped set yields 2 iterations for {5,7}.
BEZEL_TEST(codegen_collection_keys, for_in_set_int_count_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(5); s.Add(7); s.Add(5); "
        "  var n := 0; "
        "  for var x in s do n := n + 1; end; "
        "  return n; "                                // 2 distinct elements
        "end Main; end C;"), 2);
}

// §4.10 / §4.6: `for var x in <Set<string>>` binds each string element; the
// element value is usable (sum of element lengths). "ab", "cde", "ab" dedup to
// {"ab","cde"}; total length 2 + 3 = 5.
BEZEL_TEST(codegen_collection_keys, for_in_set_string_value_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Set<string>(); "
        "  s.Add(\"ab\"); s.Add(\"cde\"); s.Add(\"ab\"); "
        "  var total := 0; "
        "  for var x in s do total := total + x.Length; end; "
        "  return total; "                            // 2 + 3 = 5
        "end Main; end C;"), 5);
}

// §4.10 / §9.1: `for var p in <Set<Point>>` binds each record element; the
// record's fields are usable in the body. {(1,2),(3,4)} → sum of x+y = 10. The
// loop variable is typed Point (sema types the set's element), so `p.x`/`p.y`
// resolve to the record fields.
BEZEL_TEST(codegen_collection_keys, for_in_set_record_fields_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<Point>(); "
        "  s.Add(new Point(1, 2)); "
        "  s.Add(new Point(3, 4)); "
        "  s.Add(new Point(1, 2)); "                  // dup → collapsed
        "  var sum := 0; "
        "  for var p in s do sum := sum + p.x + p.y; end; "
        "  return sum; "                              // (1+2) + (3+4) = 10
        "end Main; end C;"), 10);
}

// --- #370: iteration over Dictionary<K,V>.Keys (§4.8) ---
//
// `Dictionary<K,V>.Keys` is `ICollection<K>` (§4.8); the spec idiom
// `for var key in dict.Keys do …` iterates the keys. A non-string key lives in
// the generic hash table, so its Keys uses bz_dict_keys_gen (not the string-key
// bz_dict_keys_str path, which would reinterpret the key bytes as a pointer).

// §4.8: `for var k in <Dictionary<int,int>>.Keys` sums the keys. Keys {3,4} → 7.
// (The string-key Keys path would have crashed on the integer keys.)
BEZEL_TEST(codegen_collection_keys, for_in_dict_int_keys_sum_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d[3] := 100; "
        "  d[4] := 200; "
        "  var sum := 0; "
        "  for var k in d.Keys do sum := sum + k; end; "
        "  return sum; "                              // 3 + 4 = 7
        "end Main; end C;"), 7);
}

// §4.8: `for var k in <Dictionary<int,int>>.Keys` iterates once per key — a
// counted loop over a 2-entry dictionary's keys yields 2 iterations.
BEZEL_TEST(codegen_collection_keys, for_in_dict_int_keys_count_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d[3] := 100; "
        "  d[4] := 200; "
        "  d[3] := 111; "                           // updates key 3, no new key
        "  var n := 0; "
        "  for var k in d.Keys do n := n + 1; end; "
        "  return n; "                                // 2 distinct keys
        "end Main; end C;"), 2);
}

// §4.8 / §4.6: `for var k in <Dictionary<string,int>>.Keys` binds each string
// key; the key value is usable (sum of key lengths). Keys {"ab","cde"} → 5.
// (This is the string-key Keys path: bz_dict_keys_str enumeration.)
BEZEL_TEST(codegen_collection_keys, for_in_dict_string_keys_value_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"ab\"] := 1; "
        "  d[\"cde\"] := 2; "
        "  var total := 0; "
        "  for var k in d.Keys do total := total + k.Length; end; "
        "  return total; "                            // 2 + 3 = 5
        "end Main; end C;"), 5);
}

// #366/#370 / §4.8 / §9.1: `for var k in <Dictionary<Point,int>>.Keys` binds
// each RECORD key; the key's fields are usable in the body (`k.y`). Previously
// `d.Keys` carried no sema element type, so the loop variable was untyped and
// `k.y` was dropped (the result collapsed to 0); the explicit `for var k: Point`
// was the workaround. Now `d.Keys` is typed as a collection-of-K, so the loop
// variable infers Point and `k.y` resolves. Keys {(1,10),(2,20)} → 10 + 20 = 30.
// (Load-bearing: drop the §4.8 Keys-element typing and `k.y` reads zero.)
BEZEL_TEST(codegen_collection_keys, for_in_dict_record_keys_field_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Point, int>(); "
        "  d[new Point(1, 10)] := 100; "
        "  d[new Point(2, 20)] := 200; "
        "  var sum := 0; "
        "  for var k in d.Keys do sum := sum + k.y; end; "
        "  return sum; "                              // 10 + 20 = 30
        "end Main; end C;"), 30);
}

// §4.10: iterating an EMPTY dict-backed Set runs zero times (no crash on the
// empty generic table — the key snapshot is length 0).
BEZEL_TEST(codegen_collection_keys, for_in_set_empty_no_iterations_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  var n := 0; "
        "  for var x in s do n := n + 1; end; "
        "  return n; "                                // 0 iterations
        "end Main; end C;"), 0);
}

// §4.10 / §21.5: `break` out of a dict-backed Set iteration stops the loop and
// runs the per-iteration cleanup correctly (the snapshot is freed at loop exit).
BEZEL_TEST(codegen_collection_keys, for_in_set_break_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(1); s.Add(2); s.Add(3); "
        "  var n := 0; "
        "  for var x in s do n := n + 1; if n = 2 then break; end; end; "
        "  return n; "                                // stops after 2
        "end Main; end C;"), 2);
}

// --- #351: record-element Set Clear() and set-algebra over the generic table ---
//
// §4.10: a `Set<T>` (T a record) is generic-hash-table-backed (#343/#370). After
// #351, Clear() empties that table and the set-algebra ops whose `other` is a
// same-typed dict-backed Set are wired by enumerating the other table's elements
// (bz_dict_keys_gen) and folding them into/against self. Before #351 these all
// fell through (returned nullptr) — so each test is load-bearing.

// §4.10: Set<record>.Clear() empties the backing table; Count is 0 afterward and
// the set is reusable (Add after Clear works).
BEZEL_TEST(codegen_collection_keys, set_record_clear_empties_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<P>(); "
        "  s.Add(new P(1, 2)); s.Add(new P(3, 4)); "
        "  s.Clear(); "
        "  s.Add(new P(5, 6)); "                       // reusable after Clear
        "  return s.Count * 10 + int(s.Contains(new P(5, 6))); "  // 1*10 + 1 = 11
        "end Main; end C;"), 11);
}

// §4.10: Set<int>.Clear() also empties (the generic table backs non-record
// element sets too — D8). Count 0 after Clear.
BEZEL_TEST(codegen_collection_keys, set_int_clear_empties_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Add(1); s.Add(2); s.Add(3); "
        "  s.Clear(); "
        "  return s.Count; "                           // 0
        "end Main; end C;"), 0);
}

// §4.10: Set<record>.UnionWith(other) adds every element of `other` not already
// present. {(1,2),(3,4)} ∪ {(3,4),(5,6)} = {(1,2),(3,4),(5,6)} → Count 3 (the
// shared (3,4) is not duplicated).
BEZEL_TEST(codegen_collection_keys, set_record_unionwith_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Set<P>(); using var b := new Set<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  b.Add(new P(3, 4)); b.Add(new P(5, 6)); "
        "  a.UnionWith(b); "
        "  return a.Count * 10 + int(a.Contains(new P(5, 6))); "  // 3*10 + 1 = 31
        "end Main; end C;"), 31);
}

// §4.10: Set<record>.ExceptWith(other) removes every element of `other` from
// self. {(1,2),(3,4),(5,6)} \ {(3,4)} = {(1,2),(5,6)} → Count 2; (3,4) gone.
BEZEL_TEST(codegen_collection_keys, set_record_exceptwith_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Set<P>(); using var b := new Set<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); a.Add(new P(5, 6)); "
        "  b.Add(new P(3, 4)); "
        "  a.ExceptWith(b); "
        "  return a.Count * 10 + int(not a.Contains(new P(3, 4))); "  // 2*10 + 1 = 21
        "end Main; end C;"), 21);
}

// §4.10: Set<record>.IsSubsetOf(other) is true iff every element of self is in
// other. {(1,2)} ⊆ {(1,2),(3,4)} → true; {(1,2),(9,9)} ⊄ {(1,2),(3,4)} → false.
BEZEL_TEST(codegen_collection_keys, set_record_issubsetof_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Set<P>(); using var b := new Set<P>(); "
        "  using var c := new Set<P>(); "
        "  a.Add(new P(1, 2)); "
        "  b.Add(new P(1, 2)); b.Add(new P(3, 4)); "
        "  c.Add(new P(1, 2)); c.Add(new P(9, 9)); "
        "  return int(a.IsSubsetOf(b)) * 10 + int(c.IsSubsetOf(b)); "  // 1*10 + 0 = 10
        "end Main; end C;"), 10);
}

// §4.10: Set<record>.IsSupersetOf(other) is true iff every element of other is
// in self. {(1,2),(3,4)} ⊇ {(1,2)} → true; ⊉ {(7,7)} → false.
BEZEL_TEST(codegen_collection_keys, set_record_issupersetof_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Set<P>(); using var b := new Set<P>(); "
        "  using var c := new Set<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  b.Add(new P(1, 2)); "
        "  c.Add(new P(7, 7)); "
        "  return int(a.IsSupersetOf(b)) * 10 + int(a.IsSupersetOf(c)); "  // 1*10 + 0 = 10
        "end Main; end C;"), 10);
}
