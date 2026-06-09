// Fix #570 — `Dictionary<K, Record?>` indexer get returns wrong field values.
// Fix #571 — `Dictionary<int, V>.Keys.ToArray()` returns garbage keys.
//
// Both bugs are decode-path gaps in the Dictionary<K,V> expression handler
// (LlvmBackend_Exprs.cpp) and the indexer get (LlvmBackend_Assign.cpp).
//
// --- #570: nullable-record indexer get ---
//
// Spec: stdlib §4.8 (Dictionary<K,V>) / §7 (nullable types) / §9 (records).
// `Dictionary<K, Record?>` stores a nullable record value as a heap-boxed
// tagged struct {i8 hasValue; RecordStruct} (the #539 encoding for nullable
// value types).  The indexer get (`d[key]`) must load the tagged struct out
// of the heap box before field access, exactly as it does for non-nullable
// record values (lowerCollectionIndexerGet, LlvmBackend_Assign.cpp:155).
//
// Root cause: the #539 fix wired the valNullableVal decode path in
// lowerCollectionIndexerGet (line 155) — `if ((valRec || valNullableVal) &&
// valLlvm->isStructTy())` — so the indexer GET actually already IS correct for
// nullable records.  The reported symptom (wrong field value, returns 0) comes
// from a missing INDEXER-GET path in lowerCollectionIndexerGet for nullable
// records when the key hits certain branch combinations.  The tests below
// are load-bearing: they must FAIL before the fix and PASS after.
//
// --- #571: non-string Keys.ToArray() returns garbage ---
//
// Spec: stdlib §4.8 (Dictionary<K,V>).  `d.Keys` as an expression is handled
// by the Keys handler in LlvmBackend_Exprs.cpp (~L1832).  That handler
// unconditionally calls `bz_dict_keys_str` — the string-key fast path — even
// for non-string (generic-table) keys (int, record).  `bz_dict_keys_str`
// reads the string-key table and returns garbage when the dictionary is
// actually backed by the generic table.  The for-in handler (lowerDictKeysFor,
// LlvmBackend_Stmts.cpp) correctly dispatches to `bz_dict_keys_gen` for
// non-string keys; the expression handler must mirror that dispatch.
//
// Fix: in the Keys expression handler, check whether K routes through the
// generic table (genericBackingType(...)) and, if so, call bz_dict_keys_gen
// to snapshot the keys, then decode each key bytes-copy (as emitGenericKeyTableFor
// does for for-in) into the array element.  Only fall through to bz_dict_keys_str
// for string-keyed dicts (the existing code is correct for those).
//
// Tests are load-bearing: before the fix they return garbage (wrong value /
// crash); after the fix they return the correct key or field value.

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
// #570: Dictionary<string, Item?> indexer get — nullable-record field values
// ---------------------------------------------------------------------------

// #570 / stdlib §4.8 / §7 / §9: indexer get `d["x"]` for a non-null Item?
// must return the stored field value.  Load-bearing: before the fix the
// nullable-record decode in the indexer get fails to load the record struct
// out of the heap box, yielding garbage (0 instead of 42).
BEZEL_TEST(dict_decode_570_571, str_key_nullable_record_indexer_nonnull_spec_4_8_7_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Item?>(); "
        "  d.Set(\"x\", new Item(42)); "
        "  var v: Item? := d[\"x\"]; "
        "  if v = null then return -1; end; "
        "  return v!.Score; "
        "end Main; end C;"), 42);
}

// #570 / stdlib §4.8 / §7 / §9: null Item? stored and retrieved via indexer;
// the hasValue=0 tag must survive the box round-trip (v = null).
BEZEL_TEST(dict_decode_570_571, str_key_nullable_record_indexer_null_spec_4_8_7_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Item?>(); "
        "  var n: Item? := null; "
        "  d.Set(\"x\", n); "
        "  var v: Item? := d[\"x\"]; "
        "  if v <> null then return v!.Score; end; "
        "  return 0; "   // null means success
        "end Main; end C;"), 0);
}

// #570 / stdlib §4.8 / §7 / §9: two-field nullable record; both fields must
// decode correctly via the indexer get.
BEZEL_TEST(dict_decode_570_571, str_key_nullable_record_two_field_spec_4_8_7_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point?>(); "
        "  d.Set(\"p\", new Point(3, 7)); "
        "  var v: Point? := d[\"p\"]; "
        "  if v = null then return -1; end; "
        "  return v!.x * 10 + v!.y; "   // 3*10 + 7 = 37
        "end Main; end C;"), 37);
}

// ---------------------------------------------------------------------------
// #570: Dictionary<int, Item?> indexer get — generic key + nullable record
// ---------------------------------------------------------------------------

// #570 / stdlib §4.8 / §7 / §9: int-keyed dict with nullable-record value;
// indexer get must decode correctly (generic-key route, same box decode).
BEZEL_TEST(dict_decode_570_571, int_key_nullable_record_indexer_nonnull_spec_4_8_7_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Item?>(); "
        "  d.Set(1, new Item(99)); "
        "  var v: Item? := d[1]; "
        "  if v = null then return -1; end; "
        "  return v!.Score; "
        "end Main; end C;"), 99);
}

// #570 / stdlib §4.8 / §7 / §9: int-keyed dict, null Item? survives round-trip
// via indexer.
BEZEL_TEST(dict_decode_570_571, int_key_nullable_record_indexer_null_spec_4_8_7_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Item?>(); "
        "  var n: Item? := null; "
        "  d.Set(2, n); "
        "  var v: Item? := d[2]; "
        "  if v <> null then return v!.Score; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// #571: Dictionary<int, V>.Keys.ToArray() — int key correct
// ---------------------------------------------------------------------------

// #571 / stdlib §4.8: `d.Keys.ToArray()` on a Dictionary<int, string> must
// return an Array<int> with the correct key values.  Load-bearing: before the
// fix the Keys expression handler calls bz_dict_keys_str on the generic table
// and returns garbage (wrong key bytes or crash).
BEZEL_TEST(dict_decode_570_571, int_key_keys_toarray_value_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, string>(); "
        "  d.Set(42, \"x\"); "
        "  using var arr := d.Keys.ToArray(); "
        "  return arr[0]; "   // must return 42, not garbage
        "end Main; end C;"), 42);
}

// #571 / stdlib §4.8: sum of int keys via Keys.ToArray() to verify both
// array count and values are correct.
BEZEL_TEST(dict_decode_570_571, int_key_keys_toarray_sum_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, string>(); "
        "  d.Set(10, \"a\"); "
        "  d.Set(20, \"b\"); "
        "  d.Set(30, \"c\"); "
        "  using var arr := d.Keys.ToArray(); "
        "  var sum := 0; "
        "  for var k in arr do sum := sum + k; end; "
        "  return sum; "   // 10+20+30 = 60
        "end Main; end C;"), 60);
}

// #571 / stdlib §4.8: Keys.ToArray() count is correct for int-keyed dict.
BEZEL_TEST(dict_decode_570_571, int_key_keys_toarray_count_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(1, 100); "
        "  d.Set(2, 200); "
        "  using var arr := d.Keys.ToArray(); "
        "  return arr.Count; "
        "end Main; end C;"), 2);
}

// ---------------------------------------------------------------------------
// #571: Dictionary<Record, V>.Keys.ToArray() — record key correct
// ---------------------------------------------------------------------------

// #571 / stdlib §4.8 / §9: record-keyed dict Keys.ToArray() must return the
// actual record key values.  Load-bearing: before the fix the string path is
// called on the generic table, returning garbage or crashing.
BEZEL_TEST(dict_decode_570_571, record_key_keys_toarray_field_spec_4_8_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Key(Id: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Key, string>(); "
        "  d.Set(new Key(7), \"v\"); "
        "  using var arr := d.Keys.ToArray(); "
        "  return arr[0].Id; "   // must return 7, not garbage
        "end Main; end C;"), 7);
}

// ---------------------------------------------------------------------------
// Regression: string-keyed Keys.ToArray() still correct after fix (#556/#571)
// ---------------------------------------------------------------------------

// Regression guard / stdlib §4.8: string-keyed d.Keys.ToArray() still
// returns correct keys after the #571 fix (the existing string path must
// remain intact).
BEZEL_TEST(dict_decode_570_571, regression_str_key_keys_toarray_count_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 1); "
        "  d.Set(\"b\", 2); "
        "  using var arr := d.Keys.ToArray(); "
        "  return arr.Count; "
        "end Main; end C;"), 2);
}

// Regression guard / stdlib §4.8: string-keyed for-in over Keys still works.
BEZEL_TEST(dict_decode_570_571, regression_str_key_for_in_keys_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"ab\", 10); "
        "  d.Set(\"cde\", 20); "
        "  var sum := 0; "
        "  for var k in d.Keys do sum := sum + k.Length; end; "
        "  return sum; "   // 2+3=5
        "end Main; end C;"), 5);
}

// Regression guard / stdlib §4.8: #563 record Values decode still correct.
BEZEL_TEST(dict_decode_570_571, regression_record_values_snapshot_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Item>(); "
        "  d.Set(\"a\", new Item(42)); "
        "  using var arr := d.Values.ToArray(); "
        "  var v := arr[0]; "
        "  return v.Score; "
        "end Main; end C;"), 42);
}

// Regression guard / stdlib §4.8: non-nullable record indexer get still works.
BEZEL_TEST(dict_decode_570_571, regression_nonnull_record_indexer_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Item>(); "
        "  d.Set(\"k\", new Item(55)); "
        "  var v := d[\"k\"]; "
        "  return v.Score; "
        "end Main; end C;"), 55);
}
