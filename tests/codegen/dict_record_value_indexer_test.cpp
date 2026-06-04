// Codegen — record-valued Dictionary indexer get/set (issue #393, deferred from
// #388). stdlib §4.8 `Dictionary<K, V>`.
//
// §4.8: `Dictionary<K, V>` is the BORROWING map; `V` is unconstrained, so the
// value type may be a record (a value/aggregate type). A record value is too
// wide for the scalar inttoptr value-slot encoding, so the runtime heap-boxes
// it (malloc + memcpy of its bytes) and stores the box pointer; on retrieval the
// box is loaded and the record bytes copied out. This boxed-record round-trip is
// already exercised through `.Set` / `.TryGetValue` (collection_keys_test.cpp,
// "dict_record_value_*"); #388 landed the heap-collection indexer but BAILED for
// record values, leaving `dict[key]` get/set for `Dictionary<K, record>`
// unlowered. This file is the spec-derived contract for that gap, and the
// indexer get/set now mirror the `Set` / `TryGetValue` boxed-record decode.
//
// Semantics under test (§4.8):
//   * `dict[key]` (get) returns the stored record value (field-by-field copy);
//     it raises `KeyNotFoundException` if the key is absent.
//   * `dict[key] := v` (set) is insert-or-replace: it boxes the record value and
//     stores the box (replacing — and freeing — any prior box for that key).
//
// OwnedDictionary<K, record> is intentionally NOT covered: §4.9 constrains
// `OwnedDictionary<K, V>` to `where V: class`, so a record (a value type) can
// never be an owning dictionary's value. There is therefore no displaced-record
// disposal path for the indexer setter to model — the borrowing `Dictionary`
// is the only collection whose indexer takes a record value. (Decline-with-
// reason: no test is written for an ill-typed `OwnedDictionary<K, record>`.)
//
// Each test is LOAD-BEARING: before the fix the indexer get/set bailed for a
// record value, so the read fell through to the inline-array path (garbage /
// wrong result) and the set was silently dropped (the value never landed). The
// `int(... )` predicate would be 0 (or the program would mis-decode) rather than
// the asserted 1; the missing-key test would not throw KeyNotFoundException.

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
// Dictionary<string, Point> — string-key fast path. §4.8.
// ---------------------------------------------------------------------------

// §4.8: `dict[key]` (get) returns the stored record value. Read into a local
// (the natural value-copy of a record get), then read its fields. Load-bearing:
// before the fix the record-valued get bailed and the read mis-decoded.
BEZEL_TEST(codegen_dict_record_value_indexer, str_key_indexer_get_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d.Add(\"a\", new Point(1, 2)); "
        "  d.Add(\"b\", new Point(3, 4)); "
        "  var p := d[\"a\"]; "
        "  var q := d[\"b\"]; "
        "  return int(p.x = 1 and p.y = 2 and q.x = 3 and q.y = 4); "
        "end Main; end C;"), 1);
}

// §4.8: `dict[key]` (get) followed immediately by field access on the read
// value (`d[key].x`) — the record get returns a struct value usable directly.
BEZEL_TEST(codegen_dict_record_value_indexer, str_key_indexer_get_field_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d.Add(\"a\", new Point(11, 22)); "
        "  return d[\"a\"].x * 100 + d[\"a\"].y; "       // 11*100 + 22 = 1122
        "end Main; end C;"), 1122);
}

// §4.8: `dict[key] := v` (set) INSERTS a record value under a fresh key; a later
// get reads it back. Load-bearing: before the fix the record-valued set was a
// no-op (the value never landed; the get would then throw / mis-decode).
BEZEL_TEST(codegen_dict_record_value_indexer, str_key_indexer_set_insert_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d[\"a\"] := new Point(5, 6); "
        "  var p := d[\"a\"]; "
        "  return int(d.Count = 1 and p.x = 5 and p.y = 6); "
        "end Main; end C;"), 1);
}

// §4.8: `dict[key] := v` (set) REPLACES an existing key's record value (insert-
// or-replace); the other key is untouched. Load-bearing: before the fix the
// replacing set was dropped, so the read would still see the old value.
BEZEL_TEST(codegen_dict_record_value_indexer, str_key_indexer_set_replace_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d.Add(\"a\", new Point(1, 2)); "
        "  d.Add(\"b\", new Point(3, 4)); "
        "  d[\"a\"] := new Point(9, 9); "                // replace a's record
        "  var p := d[\"a\"]; var q := d[\"b\"]; "
        "  return int(d.Count = 2 and p.x = 9 and p.y = 9 "
        "         and q.x = 3 and q.y = 4); "
        "end Main; end C;"), 1);
}

// §4.8: round-trip purely through the indexer (set then get, no method calls).
BEZEL_TEST(codegen_dict_record_value_indexer, str_key_indexer_set_then_get_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d[\"k\"] := new Point(7, 8); "
        "  d[\"k\"] := new Point(70, 80); "             // replace via indexer
        "  var p := d[\"k\"]; "
        "  return p.x + p.y; "                          // 70 + 80 = 150
        "end Main; end C;"), 150);
}

// ---------------------------------------------------------------------------
// Dictionary<int, Point> — generic-key path (bz_dict_*_gen). §4.8.
// ---------------------------------------------------------------------------

// §4.8: an int-keyed record-valued dictionary routes the key through the
// generic-key runtime; the indexer get must decode the boxed record the same
// way. Load-bearing: before the fix this bailed for the record value.
BEZEL_TEST(codegen_dict_record_value_indexer, int_key_indexer_set_get_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Point>(); "
        "  d[1] := new Point(1, 2); "
        "  d[2] := new Point(3, 4); "
        "  d[1] := new Point(9, 9); "                   // replace key 1
        "  var p := d[1]; var q := d[2]; "
        "  return int(d.Count = 2 and p.x = 9 and p.y = 9 "
        "         and q.x = 3 and q.y = 4); "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// Missing key — §4.8: `dict[key]` raises KeyNotFoundException if absent.
// ---------------------------------------------------------------------------

// §4.8: a record-valued get for an ABSENT key raises KeyNotFoundException (the
// same throw-on-absent guard the scalar/pointer indexer get uses). The throw is
// caught and a sentinel returned. Load-bearing: before the fix the record get
// bailed and fell to the inline-array path (no throw → garbage), so the catch
// would not fire.
BEZEL_TEST(codegen_dict_record_value_indexer, str_key_missing_throws_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d.Add(\"a\", new Point(1, 2)); "
        "  try "
        "    var p := d[\"missing\"]; "
        "    return p.x; "                              // unreachable
        "  catch KeyNotFoundException => return 42; "
        "  end; "
        "  return -1; "
        "end Main; end C;"), 42);
}

// §4.8 (generic key): the missing-key throw also fires on the int-keyed path.
BEZEL_TEST(codegen_dict_record_value_indexer, int_key_missing_throws_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Point>(); "
        "  d[1] := new Point(1, 2); "
        "  try "
        "    var p := d[99]; "
        "    return p.x; "                              // unreachable
        "  catch KeyNotFoundException => return 7; "
        "  end; "
        "  return -1; "
        "end Main; end C;"), 7);
}
