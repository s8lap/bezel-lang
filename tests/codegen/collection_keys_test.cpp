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

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
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
        "  d.Set(1, 100); "
        "  d.Set(2, 200); "
        "  d.Set(1, 111); "                       // updates key 1, no new entry
        "  var v := 0; d.TryGetValue(1, out v); "
        "  var w := 0; d.TryGetValue(2, out w); "
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
        "    d.Set(7, 70); "
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
        "  d.Set(5, 100); "
        "  d.Set(2 + 3, 999); "                   // equal key, distinct build
        "  var v := 0; d.TryGetValue(5, out v); "
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
        "  d.Set(Color.Red, 50); "
        "  d.Set(Color.Blue, 70); "
        "  var v := 0; d.TryGetValue(Color.Blue, out v); "
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
        "  d.Set(\"a\", new Point(1, 2)); "
        "  d.Set(\"b\", new Point(3, 4)); "
        "  var p := new Point(0, 0); "
        "  d.TryGetValue(\"a\", out p); "
        "  var q := new Point(0, 0); "
        "  d.TryGetValue(\"b\", out q); "
        "  return int(d.Count = 2 and p.x = 1 and p.y = 2 "
        "         and q.x = 3 and q.y = 4); "
        "end Main; end C;"), 1);
}

// §4.8: a record value's fields are independent per key (distinct boxes). Updating
// a key's record value replaces it; the other key's value is untouched.
BEZEL_TEST(codegen_collection_keys, dict_record_value_update_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d.Set(\"a\", new Point(1, 2)); "
        "  d.Set(\"b\", new Point(3, 4)); "
        "  d.Set(\"a\", new Point(9, 9)); "       // update a's record value
        "  var p := new Point(0, 0); d.TryGetValue(\"a\", out p); "
        "  var q := new Point(0, 0); d.TryGetValue(\"b\", out q); "
        "  return int(d.Count = 2 and p.x = 9 and p.y = 9 "
        "         and q.x = 3 and q.y = 4); "
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
