// Fix #539 — Dictionary<K, T?> where T? is a nullable value type crashes
// with LLVM IR-verification failure ("ZExt only operates on integer / zext
// {i8,i32} to i64" on Set, "Trunc only produces integer / trunc i64 to
// {i8,i32}" on TryGetValue/indexer-get).
//
// Spec: stdlib §4.8 (Dictionary<K,V>) — V is unconstrained, so V may be a
// nullable VALUE type (int?, bool?, enum?, nullable record), which lowers to a
// tagged struct {i8 hasValue; T}.  The generic-dict value slot encoding (see
// LlvmBackend_Calls.cpp, issue D4) must treat such structs the same way it
// treats aggregate record values: heap-box the struct on store, load it back
// on retrieval.  Before the fix, nullable value types fell through the record
// check (valRec was null) to the scalar ZExt/Trunc path, which is illegal on
// struct operands and causes an IR-verification crash (SIGILL) at emitModule.
//
// §7 (nullable types): nullable value types have BOTH a null representation
// (hasValue=0) and a non-null representation (hasValue=1, value=T).  The tag
// byte must survive the box round-trip unchanged so that null vs non-null is
// correctly recovered after a Set/TryGetValue/GetOrAdd/indexer sequence.
//
// Root cause: encodeValue (Set/Add/GetOrAdd/indexer-set) and decodeValue
// (TryGetValue/GetOrAdd/indexer-get/Values-for) only checked valRec (a
// non-nullable Named-Record TypeRef); isNullableValueTy structs slipped to the
// ZExt path.  The fix adds a valNullableVal guard that routes nullable value
// types through the same heap-box path as records.

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
// Dictionary<string, int?> — nullable int value, string key
// ---------------------------------------------------------------------------

// #539 / stdlib §4.8 / §7: Set a non-null int? variable then TryGetValue
// recovers it.  The tag byte must be preserved through the box round-trip.
// Bezel syntax: `=` is equality, `<>` is not-equal, `<> null` tests non-null.
BEZEL_TEST(dict_nullable_value_539, str_key_nonnull_int_set_tryget_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var v: int? := 42; "
        "  d.Set(\"x\", v); "
        "  var vx: int? := null; "
        "  var found := d.TryGetValue(\"x\", out vx); "
        "  if not found then return -1; end; "
        "  if vx = null then return -2; end; "
        "  return int(vx!); "
        "end Main; end C;"), 42);
}

// #539 / stdlib §4.8 / §7: Set a null int? variable then TryGetValue returns
// the null value (hasValue=0 preserved).
BEZEL_TEST(dict_nullable_value_539, str_key_null_int_set_tryget_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var vn: int? := null; "
        "  d.Set(\"x\", vn); "
        "  var vx: int? := 99; "
        "  var found := d.TryGetValue(\"x\", out vx); "
        "  if not found then return -1; end; "
        "  if vx <> null then return int(vx!); end; "
        "  return 0; "      // null means success
        "end Main; end C;"), 0);
}

// #539 / stdlib §4.8 / §7: indexer set d["x"] := v and indexer get d["x"]
// for a non-null int?.
BEZEL_TEST(dict_nullable_value_539, str_key_nonnull_int_indexer_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var v: int? := 17; "
        "  d[\"k\"] := v; "
        "  var got: int? := d[\"k\"]; "
        "  if got = null then return -1; end; "
        "  return int(got!); "
        "end Main; end C;"), 17);
}

// #539 / stdlib §4.8 / §7: indexer set + get for a null int?.
BEZEL_TEST(dict_nullable_value_539, str_key_null_int_indexer_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var vn: int? := null; "
        "  d[\"k\"] := vn; "
        "  var got: int? := d[\"k\"]; "
        "  if got <> null then return int(got!); end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// #539 / stdlib §4.8 / §7: iterate .Values — sum only the non-null int?
// entries; null entries contribute 0.
BEZEL_TEST(dict_nullable_value_539, str_key_int_nullable_values_iter_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var a: int? := 10; "
        "  var b: int? := null; "
        "  var c: int? := 30; "
        "  d.Set(\"a\", a); "
        "  d.Set(\"b\", b); "
        "  d.Set(\"c\", c); "
        "  var sum := 0; "
        "  for var v in d.Values do "
        "    if v <> null then sum := sum + int(v!); end; "
        "  end; "
        "  return sum; "       // 10 + 30 = 40
        "end Main; end C;"), 40);
}

// #539 / stdlib §4.8 / §7: Count is correct after mixing null and non-null
// int? entries.
BEZEL_TEST(dict_nullable_value_539, str_key_int_nullable_count_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var a: int? := 1; "
        "  var b: int? := null; "
        "  d.Set(\"a\", a); "
        "  d.Set(\"b\", b); "
        "  return d.Count; "
        "end Main; end C;"), 2);
}

// ---------------------------------------------------------------------------
// Dictionary<int, bool?> — nullable bool value, generic (int) key
// ---------------------------------------------------------------------------

// #539 / stdlib §4.8 / §7: int key, bool? value — Set non-null then recover.
BEZEL_TEST(dict_nullable_value_539, int_key_nonnull_bool_set_tryget_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, bool?>(); "
        "  var bv: bool? := true; "
        "  d.Set(1, bv); "
        "  var out_bv: bool? := null; "
        "  var found := d.TryGetValue(1, out out_bv); "
        "  if not found then return -1; end; "
        "  if out_bv = null then return -2; end; "
        "  return (1 if out_bv! else 0); "
        "end Main; end C;"), 1);
}

// #539 / stdlib §4.8 / §7: int key, bool? value — Set null bool? then recover.
BEZEL_TEST(dict_nullable_value_539, int_key_null_bool_set_tryget_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, bool?>(); "
        "  var bv: bool? := null; "
        "  d.Set(2, bv); "
        "  var out_bv: bool? := true; "
        "  var found := d.TryGetValue(2, out out_bv); "
        "  if not found then return -1; end; "
        "  if out_bv <> null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// #539 / stdlib §4.8 / §7: int key, bool? value — mix null and non-null,
// sum 1 per true entry, 0 per null or false.
BEZEL_TEST(dict_nullable_value_539, int_key_bool_nullable_mix_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, bool?>(); "
        "  var t: bool? := true; "
        "  var f: bool? := false; "
        "  var n: bool? := null; "
        "  d.Set(1, t); "
        "  d.Set(2, f); "
        "  d.Set(3, n); "
        "  var sum := 0; "
        "  for var v in d.Values do "
        "    if v <> null then "
        "      if v! then sum := sum + 1; end; "
        "    end; "
        "  end; "
        "  return sum; "       // only entry 1 (true) contributes
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// CONTROL: non-nullable value (scalar int) still works — regression guard
// ---------------------------------------------------------------------------

// #539 CONTROL / stdlib §4.8: Dictionary<string, int> (scalar, non-nullable)
// still round-trips correctly — the fix must not regress the scalar path.
BEZEL_TEST(dict_nullable_value_539, control_scalar_int_unaffected_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 100); "
        "  d.Set(\"b\", 200); "
        "  var v := 0; "
        "  var found := d.TryGetValue(\"a\", out v); "
        "  if not found then return -1; end; "
        "  return v; "
        "end Main; end C;"), 100);
}

// #539 CONTROL / stdlib §4.8: Dictionary<string, SomeRecord> (record value)
// still round-trips — the fix routes nullable values to the same box path and
// must not break the existing record path.
BEZEL_TEST(dict_nullable_value_539, control_record_value_unaffected_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(a: int, b: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Pair>(); "
        "  d.Set(\"p\", new Pair(3, 7)); "
        "  var pv: Pair := new Pair(0, 0); "
        "  var found := d.TryGetValue(\"p\", out pv); "
        "  if not found then return -1; end; "
        "  return pv.a + pv.b; "
        "end Main; end C;"), 10);
}

// #539 CONTROL / stdlib §4.8: Dictionary<string, string> (pointer value)
// still round-trips — the fix must not disturb the pointer path.
BEZEL_TEST(dict_nullable_value_539, control_string_value_unaffected_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, string>(); "
        "  d.Set(\"k\", \"hello\"); "
        "  var s := \"\"; "
        "  var found := d.TryGetValue(\"k\", out s); "
        "  if not found then return -1; end; "
        "  return s.Length; "
        "end Main; end C;"), 5);
}
