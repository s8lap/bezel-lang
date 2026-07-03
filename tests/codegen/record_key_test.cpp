// Codegen — records (and other non-string value types) usable as Dictionary /
// Set KEYS (issue #343, stdlib §4.8 Dictionary<K,V> / §4.10 Set<T>).
//
// §4.8: "K must provide consistent Equals and GetHashCode." A record has a
// synthesized GetHashCode (§8.3, #341) and synthesized structural Equals
// including string fields compared by content (§9.1/§9.2, #346/#349). When the
// key type is a record, the dictionary is lowered to the generic-key runtime
// variant (bz_dict_alloc_generic), parameterized by trampolines over the key's
// synthesized hash / equality. The string-key fast path is unchanged.
//
// The hash/equality contract (§8.3 hash-equality invariant) is what makes two
// equal-but-distinctly-constructed records land in the SAME hash entry: the
// hash agrees with structural equality, so lookups by an equal key succeed even
// though the key instance differs from the one originally inserted.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §4.8: Set a record key then TryGetValue with an equal key round-trips the
// value. The looked-up Point is a fresh instance (distinct allocation from the
// one inserted), so success depends on structural equality, not identity.
BEZEL_TEST(codegen_record_key, dict_set_then_get_roundtrip_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Point, int>(); "
        "  d[new Point(1, 2)] := 100; "
        "  d[new Point(3, 4)] := 200; "
        "  var v: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(new Point(1, 2), out v); "
        "  return v!; "
        "end Main; end C;"), 100);
}

// §4.8: the second key's value is retrievable too (independent entries).
BEZEL_TEST(codegen_record_key, dict_get_second_key_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Point, int>(); "
        "  d[new Point(1, 2)] := 100; "
        "  d[new Point(3, 4)] := 200; "
        "  var v: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(new Point(3, 4), out v); "
        "  return v!; "
        "end Main; end C;"), 200);
}

// §4.8: ContainsKey is true for a present (equal) key, false for an absent one.
BEZEL_TEST(codegen_record_key, dict_containskey_true_and_false_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Point, int>(); "
        "  d[new Point(1, 2)] := 100; "
        "  return int(d.ContainsKey(new Point(1, 2)) "
        "         and not d.ContainsKey(new Point(9, 9))); "
        "end Main; end C;"), 1);
}

// §8.3 hash-equality invariant: two equal Points (one built by arithmetic so the
// field-producing path differs) collapse to ONE entry — the second Set updates
// the first's value rather than adding a new entry. Count stays 1.
BEZEL_TEST(codegen_record_key, dict_equal_keys_same_entry_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Point, int>(); "
        "  d[new Point(1, 2)] := 100; "
        "  d[new Point(0 + 1, 1 + 1)] := 999; "  // equal key, distinct build
        "  var v: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(new Point(1, 2), out v); "
        "  return int(d.Count = 1 and v = 999); "
        "end Main; end C;"), 1);
}

// §4.8: different keys are distinct entries — Count reflects both.
BEZEL_TEST(codegen_record_key, dict_different_keys_distinct_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Point, int>(); "
        "  d[new Point(1, 2)] := 100; "
        "  d[new Point(2, 1)] := 200; "  // (2,1) <> (1,2): order matters
        "  return int(d.Count = 2); "
        "end Main; end C;"), 1);
}

// §4.8 / §4.6: Remove deletes a present key (by equality) and reports true; a
// subsequent ContainsKey is false and Count drops.
BEZEL_TEST(codegen_record_key, dict_remove_by_equal_key_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Point, int>(); "
        "  d[new Point(1, 2)] := 100; "
        "  d[new Point(3, 4)] := 200; "
        "  var removed := d.Remove(new Point(1, 2)); "
        "  return int(removed and d.Count = 1 "
        "         and not d.ContainsKey(new Point(1, 2)) "
        "         and d.ContainsKey(new Point(3, 4))); "
        "end Main; end C;"), 1);
}

// §8.3 / §9.1 (#346): a record with a STRING field as key. String fields hash
// and compare by CONTENT, so a key built with a concatenated string (distinct
// heap allocation) matches the inserted key. Exercises the string-field path of
// the synthesized hash/equality the trampolines call.
BEZEL_TEST(codegen_record_key, dict_record_with_string_field_key_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Tag(name: string, id: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Tag, int>(); "
        "  d[new Tag(\"alpha\", 1)] := 42; "
        "  var v: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(new Tag(\"al\" + \"pha\", 1), out v); "  // distinct heap str
        "  return int(v = 42 and d.Count = 1 "
        "         and d.ContainsKey(new Tag(\"alpha\", 1)) "
        "         and not d.ContainsKey(new Tag(\"beta\", 1))); "
        "end Main; end C;"), 1);
}

// §8.3: a record string-field key whose string content differs is a DISTINCT
// key (two entries), confirming the string field participates in the key.
BEZEL_TEST(codegen_record_key, dict_string_field_distinguishes_keys_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Tag(name: string, id: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Tag, int>(); "
        "  d[new Tag(\"x\", 1)] := 10; "
        "  d[new Tag(\"y\", 1)] := 20; "  // same id, different name
        "  var vx: int? := null; var vy: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(new Tag(\"x\", 1), out vx); "
        "  d.TryGetValue(new Tag(\"y\", 1), out vy); "
        "  return int(d.Count = 2 and vx = 10 and vy = 20); "
        "end Main; end C;"), 1);
}

// §9.2: a body record with auto-properties is also a valid key (its synthesized
// hash/equality drive the trampolines, same as a positional record).
BEZEL_TEST(codegen_record_key, dict_body_record_key_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Coord "
        "  public property X: int; "
        "  public property Y: int; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end Coord; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Coord, int>(); "
        "  d[new Coord(5, 6)] := 77; "
        "  var v: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(new Coord(5, 6), out v); "
        "  return int(v = 77 and d.ContainsKey(new Coord(5, 6))); "
        "end Main; end C;"), 1);
}

// --- Set<T> with a record element (§4.10) ---

// §4.10: Set<Point> Add then Contains by an equal (distinct) element succeeds;
// an absent element is not contained. Set membership follows the element's
// synthesized hash/equality (§8.3), same machinery as the dictionary key.
BEZEL_TEST(codegen_record_key, set_add_contains_record_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<Point>(); "
        "  s.Add(new Point(1, 2)); "
        "  s.Add(new Point(3, 4)); "
        "  return int(s.Contains(new Point(1, 2)) "
        "         and s.Contains(new Point(3, 4)) "
        "         and not s.Contains(new Point(5, 6))); "
        "end Main; end C;"), 1);
}

// §4.10: Set dedup — adding an equal (distinct) element does not grow the set;
// Count reflects one entry. Then Remove drops it.
BEZEL_TEST(codegen_record_key, set_dedup_and_remove_record_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<Point>(); "
        "  s.Add(new Point(1, 2)); "
        "  s.Add(new Point(0 + 1, 1 + 1)); "  // equal element, distinct build
        "  var c1 := s.Count; "
        "  var removed := s.Remove(new Point(1, 2)); "
        "  return int(c1 = 1 and removed and s.Count = 0); "
        "end Main; end C;"), 1);
}
