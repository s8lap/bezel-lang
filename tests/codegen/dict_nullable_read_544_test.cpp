// Fix #544 — Dictionary<K, V?> indexer-read + TryGetValue SIGSEGV when the
// value is inserted via Add (or a literal argument to Set/indexer-set) and then
// read back via the indexer or TryGetValue.
//
// Spec: stdlib §4.8 (Dictionary<K,V>) — V is unconstrained, so V may be a
// nullable VALUE type (int?, bool?, enum?, nullable record V?), which lowers
// to a tagged struct {i8 hasValue; T}.  #539 fixed the encode/decode paths for
// the case where the argument already IS a struct (e.g. `var v: int? := 42;
// d["x"] := v` — `v` lowers as the {i8,i32} struct and encodeValue boxes
// it).  However, when the argument is a non-nullable SCALAR LITERAL (e.g.
// `d.Add("a", 42)` where 42 lowers as bare i32) or a null literal (`null`
// lowers as a null pointer), encodeValue's guard
//   `if (valNullableVal && v->getType()->isStructTy())`
// is false — the value is not a struct — so it falls through to the scalar
// inttoptr path and stores the raw integer as a void*.  On decode the indexer-
// get / TryGetValue path does `CreateLoad({i8,i32}, valPtr)` where valPtr is
// the inttoptr'd integer, not a real heap box — SIGSEGV.
//
// Fix (§4.8 / §7): in every encodeValue lambda (LlvmBackend_Calls.cpp for
// Add/Set and GetOrAdd; LlvmBackend_Assign.cpp for the indexer-set), when
// valNullableVal is true but the lowered value is NOT already a struct, promote
// it to the {i8,T} tagged struct FIRST (wrapNullableValue for a scalar, or the
// zero struct for a null pointer), THEN box the struct into a heap box as the
// existing path does.  Decode paths are unchanged (they already handle the
// struct-from-box case introduced in #539).
//
// §7 (nullable types): the promotion must set hasValue=1 for any non-null
// scalar and hasValue=0 for the null literal, so that null vs non-null survives
// the round-trip (hasValue byte is stored in the malloc'd box).
//
// Root cause of the gap vs #539: #539's tests used `var v: int? := 42;
// d["x"] := v` — the sema type of `v` is already int?, so lowerExpr emits
// the tagged struct directly.  But `d.Add("a", 42)` passes a bare integer
// literal that lowerExpr emits as i32; encodeValue saw a non-struct and fell
// through to the ZExt/inttoptr path, bypassing the box.

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
// PRIMARY BUG: Dictionary<string, int?> — Add(k, literal) + indexer-get
// ---------------------------------------------------------------------------

// #544 / stdlib §4.8 / §7: d.Add("a", 42) then d["a"] crashed with SIGSEGV
// before the fix.  The integer literal 42 must be promoted to {i8=1, i32=42}
// before boxing so the decode path finds a valid {i8,i32} struct in the box.
BEZEL_TEST(dict_nullable_read_544, add_literal_int_indexer_get_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  d.Add(\"a\", 42); "
        "  var v: int? := d[\"a\"]; "
        "  if v = null then return -1; end; "
        "  return int(v!); "
        "end Main; end C;"), 42);
}

// #544 / stdlib §4.8 / §7: Add with a null int? variable then indexer-get
// returns null (hasValue=0 survives the box round-trip).
BEZEL_TEST(dict_nullable_read_544, add_null_int_indexer_get_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var nv: int? := null; "
        "  d.Add(\"b\", nv); "
        "  var got: int? := d[\"b\"]; "
        "  if got <> null then return int(got!); end; "
        "  return 0; "   // null -> success
        "end Main; end C;"), 0);
}

// #544 / stdlib §4.8 / §7: Add(literal) then TryGetValue — the twin of the
// indexer-get case; both use the same decode path (load from box) and both
// crashed before the fix.
BEZEL_TEST(dict_nullable_read_544, add_literal_int_trygetvalue_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  d.Add(\"x\", 17); "
        "  var out_v: int? := null; "
        "  var found := d.TryGetValue(\"x\", out out_v); "
        "  if not found then return -1; end; "
        "  if out_v = null then return -2; end; "
        "  return int(out_v!); "
        "end Main; end C;"), 17);
}

// #544 / stdlib §4.8 / §7: null value Add then TryGetValue — found=true,
// value=null (hasValue=0 preserved in the box).
BEZEL_TEST(dict_nullable_read_544, add_null_trygetvalue_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var nv: int? := null; "
        "  d.Add(\"y\", nv); "
        "  var out_v: int? := 99; "
        "  var found := d.TryGetValue(\"y\", out out_v); "
        "  if not found then return -1; end; "
        "  if out_v <> null then return int(out_v!); end; "
        "  return 0; "  // null -> success
        "end Main; end C;"), 0);
}

// #544 / stdlib §4.8 / §7: indexer-set with a literal (`d["k"] := 7`) then
// indexer-get — mirrors the Add path; encodeValue in lowerCollectionIndexerSet
// has the same scalar-vs-struct gap.
BEZEL_TEST(dict_nullable_read_544, indexer_set_literal_indexer_get_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  d[\"k\"] := 7; "
        "  var got: int? := d[\"k\"]; "
        "  if got = null then return -1; end; "
        "  return int(got!); "
        "end Main; end C;"), 7);
}

// #544 / stdlib §4.8 / §7: null via indexer-set then indexer-get returns null.
BEZEL_TEST(dict_nullable_read_544, indexer_set_null_indexer_get_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var nv: int? := null; "
        "  d[\"k\"] := nv; "
        "  var got: int? := d[\"k\"]; "
        "  if got <> null then return int(got!); end; "
        "  return 0; "  // null -> success
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// enum? value via Add — scalar enum constant literal
// ---------------------------------------------------------------------------

// #544 / stdlib §4.8 / §7 / §10: enum? value inserted via Add(k, EnumVal)
// — the enum constant lowers as an i32 scalar, same gap as int?.
// Color.Green = 1, so the returned value must be Color.Green (int rep = 1).
BEZEL_TEST(dict_nullable_read_544, add_enum_literal_indexer_get_spec_4_8_7_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Color?>(); "
        "  d.Add(\"c\", Color.Green); "
        "  var got: Color? := d[\"c\"]; "
        "  if got = null then return -1; end; "
        "  return int(got!); "   // Color.Green = 1
        "end Main; end C;"), 1);
}

// #544 / stdlib §4.8 / §7 / §10: null enum? round-trip via Add + TryGetValue.
BEZEL_TEST(dict_nullable_read_544, add_null_enum_trygetvalue_spec_4_8_7_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Color?>(); "
        "  var nc: Color? := null; "
        "  d.Add(\"n\", nc); "
        "  var out_c: Color? := Color.Red; "
        "  var found := d.TryGetValue(\"n\", out out_c); "
        "  if not found then return -1; end; "
        "  if out_c <> null then return int(out_c!); end; "
        "  return 0; "  // null -> success
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// Dictionary<string, Point?> — nullable record value via Add + indexer-get
// ---------------------------------------------------------------------------

// #544 / stdlib §4.8 / §7 / §9: Point? value inserted via var then Add —
// both Add and Set must produce identical heap boxes; the decode path reads
// the {i8, Point} struct from the box. Point.X=3, Point.Y=4 -> X*10+Y = 34.
BEZEL_TEST(dict_nullable_read_544, add_record_nullable_indexer_get_spec_4_8_7_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point?>(); "
        "  var p: Point? := new Point(3, 4); "
        "  d.Add(\"p\", p); "
        "  var got: Point? := d[\"p\"]; "
        "  if got = null then return -1; end; "
        "  return got!.X * 10 + got!.Y; "  // 34
        "end Main; end C;"), 34);
}

// #544 / stdlib §4.8 / §7 / §9: GetOrAdd with a nullable record? value — the
// wrong-field bug in #544 (GetOrAdd's decodeSlot loaded from the wrong offset
// when the value was not promoted to the {i8,record} struct before boxing).
// HIT path: key "p" already present; factory NOT called; decodeSlot reads
// stored box -> X*10+Y = 56.
BEZEL_TEST(dict_nullable_read_544, get_or_add_record_nullable_hit_spec_4_8_7_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point?>(); "
        "  var p: Point? := new Point(5, 6); "
        "  d.Add(\"p\", p); "
        "  var result: Point? := d.GetOrAdd(\"p\", (k) => new Point(9, 9)); "
        "  if result = null then return -1; end; "
        "  return result!.X * 10 + result!.Y; "  // 56 (hit, factory not called)
        "end Main; end C;"), 56);
}

// ---------------------------------------------------------------------------
// Mixed null/non-null with Add — iterate .Values, count + sum
// ---------------------------------------------------------------------------

// #544 / stdlib §4.8 / §7: mix Add(k, literal) and Add(k, null-var) then
// iterate .Values.  Only non-null entries contribute to the sum. sum = 40.
BEZEL_TEST(dict_nullable_read_544, add_mixed_values_iter_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var nv: int? := null; "
        "  d.Add(\"a\", 10); "
        "  d.Add(\"b\", nv); "
        "  d.Add(\"c\", 30); "
        "  var sum := 0; "
        "  for var v in d.Values do "
        "    if v <> null then sum := sum + int(v!); end; "
        "  end; "
        "  return sum; "   // 40
        "end Main; end C;"), 40);
}

// #544 / stdlib §4.8 / §7: Count is correct after mixed Add calls.
BEZEL_TEST(dict_nullable_read_544, add_mixed_count_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var nv: int? := null; "
        "  d.Add(\"a\", 5); "
        "  d.Add(\"b\", nv); "
        "  return d.Count; "  // 2
        "end Main; end C;"), 2);
}

// ---------------------------------------------------------------------------
// CONTROL: non-nullable dict and #539 Set-based paths still work
// ---------------------------------------------------------------------------

// #544 CONTROL / stdlib §4.8: Dictionary<string, int> (non-nullable, scalar)
// Add + indexer-get still works — the fix must not touch the scalar path.
BEZEL_TEST(dict_nullable_read_544, control_scalar_int_add_indexer_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 100); "
        "  d.Add(\"b\", 200); "
        "  return d[\"a\"] + d[\"b\"]; "  // 300
        "end Main; end C;"), 300);
}

// #544 CONTROL / stdlib §4.8: Dictionary<string, Point> (non-nullable record)
// Add + indexer-get still yields correct fields — the fix must not regress the
// existing record boxing path.
BEZEL_TEST(dict_nullable_read_544, control_record_add_indexer_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, Point>(); "
        "  d.Add(\"p\", new Point(3, 7)); "
        "  var pv: Point := d[\"p\"]; "
        "  return pv.X + pv.Y; "  // 10
        "end Main; end C;"), 10);
}

// #544 CONTROL: #539's Set-based nullable paths still pass (regression guard).
// var v: int? := 42; d["x"] := v; then TryGetValue -> 42.
BEZEL_TEST(dict_nullable_read_544, control_539_set_var_tryget_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var v: int? := 42; "
        "  d[\"x\"] := v; "
        "  var vx: int? := null; "
        "  var found := d.TryGetValue(\"x\", out vx); "
        "  if not found then return -1; end; "
        "  if vx = null then return -2; end; "
        "  return int(vx!); "
        "end Main; end C;"), 42);
}

// #544 CONTROL: #539's indexer set via typed var still works.
// var v: int? := 17; d["k"] := v; d["k"] -> 17.
BEZEL_TEST(dict_nullable_read_544, control_539_indexer_var_get_spec_4_8_7) {
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
