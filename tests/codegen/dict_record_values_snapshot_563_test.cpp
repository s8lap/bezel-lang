// Fix #563 — `Dictionary<K, RecordType>.Values` snapshot yields garbage field
// values for record-typed values.  Both `.ToArray()` and `for var v in d.Values`
// are broken: the snapshot path stored a heap-box POINTER into the loop/array
// slot instead of loading the actual record struct out of the box first.
//
// Spec: stdlib §4.8 (Dictionary<K,V>.Values).  The Values view is an
// ICollection<V>; iteration and .ToArray() snapshot the per-slot void* values
// from the runtime table.  For record-typed V the runtime stores a heap-boxed
// copy of the struct (malloc + memcpy) — the same encoding used by Set and Add.
// On read-back the box pointer must be dereferenced (CreateLoad of the record's
// struct type) before the struct bytes are stored into the loop variable / array
// element slot.  int and string values are correct because they don't go through
// the boxed-record path; this bug is ONLY the record (and nullable-record) decode.
//
// Root cause: lowerDictValuesFor (LlvmBackend_Stmts.cpp) allocated the loop-var
// slot as ptrTy() for records and stored the raw box pointer into it; field access
// via fieldAddr then GEP'd into the pointer-holding alloca rather than into the
// struct, yielding garbage.  The Values expression handler (LlvmBackend_Exprs.cpp,
// fix #556) had the same bug: it stored the box pointer into a ptrTy() elemSlot
// and called bz_array_add with the struct's byte-count, so bz_array_add read
// struct-size bytes out of an 8-byte pointer slot — garbage.
//
// Fix: for record (and nullable-value-struct) V, load the struct from the box
// pointer (CreateLoad(valLlvm, slotV)) and store the struct value into a slot
// typed as valLlvm — exactly mirroring the Keys path (emitGenericKeyTableFor)
// and the direct indexer get (lowerCollectionIndexerGet §4.8).
//
// Tests are load-bearing: before the fix they return garbage (non-42 / non-sum).
// After the fix they must all pass.

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

// ---------------------------------------------------------------------------
// §4.8: Dict<string, record>.Values.ToArray() — field values correct
// ---------------------------------------------------------------------------

// #563 / stdlib §4.8: Dict<string, Item>.Values.ToArray() followed by
// arr[0].Score must return the stored field value (42), not garbage.
// Load-bearing: before the fix the Values expression handler stored the box
// pointer into a ptrTy() slot and bz_array_add copied the wrong bytes.
BEZEL_TEST(dict_record_values_snapshot_563, str_key_to_array_field_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Item>(); "
        "  d[\"a\"] := new Item(42); "
        "  using var arr := d.Values.ToArray(); "
        "  var v := arr[0]; "
        "  return v.Score; "
        "end Main; end C;"), 42);
}

// #563 / stdlib §4.8: multiple record values; sum their fields via ToArray to
// verify each element is decoded correctly.
BEZEL_TEST(dict_record_values_snapshot_563, str_key_to_array_sum_fields_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Item>(); "
        "  d[\"a\"] := new Item(10); "
        "  d[\"b\"] := new Item(20); "
        "  d[\"c\"] := new Item(30); "
        "  using var arr := d.Values.ToArray(); "
        "  var sum := 0; "
        "  for var v in arr do sum := sum + v.Score; end; "
        "  return sum; "
        "end Main; end C;"), 60);
}

// #563 / stdlib §4.8: two-field record; both fields must decode correctly.
BEZEL_TEST(dict_record_values_snapshot_563, str_key_to_array_two_field_record_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d[\"p\"] := new Point(3, 7); "
        "  using var arr := d.Values.ToArray(); "
        "  var pt := arr[0]; "
        "  return pt.x * 10 + pt.y; "   // 3*10+7 = 37
        "end Main; end C;"), 37);
}

// ---------------------------------------------------------------------------
// §4.8: for var v in Dict<string, record>.Values — field values correct
// ---------------------------------------------------------------------------

// #563 / stdlib §4.8: `for var v in d.Values` where V is a record type;
// v.Score in the body must return the stored field value, not garbage.
// Load-bearing: before the fix lowerDictValuesFor stored the raw heap-box
// pointer and fieldAddr GEP'd into the pointer-holding alloca instead of
// the struct.
BEZEL_TEST(dict_record_values_snapshot_563, str_key_for_in_field_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Item>(); "
        "  d[\"x\"] := new Item(99); "
        "  var result := 0; "
        "  for var v in d.Values do result := v.Score; end; "
        "  return result; "
        "end Main; end C;"), 99);
}

// #563 / stdlib §4.8: sum over record Values via for-in; multiple entries.
BEZEL_TEST(dict_record_values_snapshot_563, str_key_for_in_sum_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Item>(); "
        "  d[\"a\"] := new Item(5); "
        "  d[\"b\"] := new Item(15); "
        "  d[\"c\"] := new Item(25); "
        "  var sum := 0; "
        "  for var v in d.Values do sum := sum + v.Score; end; "
        "  return sum; "
        "end Main; end C;"), 45);
}

// ---------------------------------------------------------------------------
// §4.8: Dict<int, record>.Values — generic-key path
// ---------------------------------------------------------------------------

// #563 / stdlib §4.8: int-keyed dict with record values; ToArray() must
// decode the record struct, not the box pointer.
BEZEL_TEST(dict_record_values_snapshot_563, int_key_to_array_field_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Item>(); "
        "  d[1] := new Item(77); "
        "  using var arr := d.Values.ToArray(); "
        "  var v := arr[0]; "
        "  return v.Score; "
        "end Main; end C;"), 77);
}

// #563 / stdlib §4.8: int-keyed dict, for-in over Values, record field correct.
BEZEL_TEST(dict_record_values_snapshot_563, int_key_for_in_field_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Item>(); "
        "  d[1] := new Item(55); "
        "  d[2] := new Item(45); "
        "  var sum := 0; "
        "  for var v in d.Values do sum := sum + v.Score; end; "
        "  return sum; "
        "end Main; end C;"), 100);
}

// ---------------------------------------------------------------------------
// §4.8: record with a string field — refcount safety
// ---------------------------------------------------------------------------

// #563 / stdlib §4.8: record carrying a string field; the string must be
// readable (correct pointer, not garbage) after the Values snapshot.  The
// string's Length is used as the observable (the field must point at the real
// bz_string object for Length to return the correct value).
// Note: the record does NOT own the string (Dictionary<K,V> is borrowing);
// the string lifetime is controlled by the caller's `greeting` local.
BEZEL_TEST(dict_record_values_snapshot_563, str_field_record_values_to_array_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Label: string, Code: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Named>(); "
        "  d[\"k\"] := new Named(\"hello\", 7); "
        "  using var arr := d.Values.ToArray(); "
        "  var n := arr[0]; "
        "  return n.Code * 10 + n.Label.Length; "  // 7*10+5 = 75
        "end Main; end C;"), 75);
}

// #563 / stdlib §4.8: for-in over Values with string-field record; same check.
BEZEL_TEST(dict_record_values_snapshot_563, str_field_record_for_in_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Label: string, Code: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Named>(); "
        "  d[\"k\"] := new Named(\"world\", 3); "
        "  var total := 0; "
        "  for var v in d.Values do total := v.Code + v.Label.Length; end; "
        "  return total; "  // 3 + 5 = 8
        "end Main; end C;"), 8);
}

// ---------------------------------------------------------------------------
// §4.8: Regression — int and string Values still correct after fix
// ---------------------------------------------------------------------------

// Regression guard: int-valued Dict<string,int>.Values.ToArray() still sums
// correctly after the record decode fix.
BEZEL_TEST(dict_record_values_snapshot_563, regression_int_values_to_array_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"a\"] := 3; "
        "  d[\"b\"] := 7; "
        "  using var arr := d.Values.ToArray(); "
        "  var sum := 0; "
        "  for var v in arr do sum := sum + v; end; "
        "  return sum; "
        "end Main; end C;"), 10);
}

// Regression guard: for-in over int Values still works.
BEZEL_TEST(dict_record_values_snapshot_563, regression_int_for_in_values_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"x\"] := 4; "
        "  d[\"y\"] := 6; "
        "  var sum := 0; "
        "  for var v in d.Values do sum := sum + v; end; "
        "  return sum; "
        "end Main; end C;"), 10);
}

// Regression guard: direct indexer for record-valued dict still correct.
BEZEL_TEST(dict_record_values_snapshot_563, regression_indexer_still_correct_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Item(Score: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Item>(); "
        "  d[\"a\"] := new Item(42); "
        "  var v := d[\"a\"]; "
        "  return v.Score; "
        "end Main; end C;"), 42);
}
