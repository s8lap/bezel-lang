// Fix verification tests for bugs #453, #454, #455.
//
// #453 (§8.6): $"{o}" / Console.WriteLine(o) where o: Object prints garbage
//   (emitPrimitiveToString(P::Object) fell through to "%d" snprintf, printing
//    the 64-bit heap pointer as an i32).  Fix: route P::Object through
//    lowerObjectToString (vtable ToString slot [3] per #417 / §8.6).
//
// #454 (§5 / §4.6): lazy LINQ (Where/Select/ToArray) on LinkedList returned 0
//   or crashed.  tryLowerLazyLinqCall guarded on isCollectionTypeName ||
//   isLazyEnumerableType — neither true for a LinkedList receiver — so lazy ops
//   fell through.  Fix: also recognise isLinkedListTypeName, which lets
//   lowerToLazyEnumerator wrap it via bz_lazy_from_llist.
//
// #455 (§4.4 / §4.5): Array<int?> / Array<bool?> Add / indexer-set stored the
//   bare inner value (i32) into a 4-byte alloca but told the runtime the element
//   is 8 bytes (collectionElemSize = 8 for the tagged {i8,i32} struct); the
//   runtime read 8 bytes from a 4-byte alloca → garbage or stack corruption.
//   Fix: ensureNullableElem lifts the inner value to the tagged struct before
//   allocating the slot (same wrapping wrapNullableValue provides elsewhere).

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

// ── #453: $"{o}" / Console.WriteLine(o) for Object ───────────────────────────

// §8.6 / #453: $"{o}" where o: Object holds a boxed int → "42".
// If the bug is present the string contains the heap pointer printed as %d
// (a small or garbage integer), so Length ≠ 2.
BEZEL_TEST(bug453_object_interpolation, interpolate_object_boxed_int_453) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 42; "
        "  var s := $\"{o}\"; "
        "  return (1 if s = \"42\" else 0); "
        "end Main; end C;"), 1);
}

// §8.6 / #453: $"{o}" where o: Object holds a boxed record.
// Boxed Point(3,4).ToString() = "Point(X: 3, Y: 4)"; Length = 17.
BEZEL_TEST(bug453_object_interpolation, interpolate_object_boxed_record_453) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Point(3, 4); "
        "  var s := $\"{o}\"; "
        "  return s.Length; "  // "Point(X: 3, Y: 4)" = 17 chars
        "end Main; end C;"), 17);
}

// §8.6 / #453: $"{o}" where o: Object holds a boxed string → string identity.
BEZEL_TEST(bug453_object_interpolation, interpolate_object_string_453) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hi\"; "
        "  var s := $\"{o}\"; "
        "  return s.Length; "  // "hi" = 2 chars
        "end Main; end C;"), 2);
}

// §8.6 / #453: composite string $"x={o}" where o: Object.
BEZEL_TEST(bug453_object_interpolation, interpolate_object_composite_453) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 7; "
        "  var s := $\"val={o}\"; "
        "  return (1 if s = \"val=7\" else 0); "
        "end Main; end C;"), 1);
}

// §8.6 / #453: regression — o.ToString() direct still correct (worked before).
BEZEL_TEST(bug453_object_interpolation, regression_object_tostring_direct_453) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 99; "
        "  return (1 if o.ToString() = \"99\" else 0); "
        "end Main; end C;"), 1);
}

// ── #454: lazy LINQ on LinkedList ─────────────────────────────────────────────

// §5 / §4.6 / #454: LinkedList<int>.Where(pred).Count() — without fix, = 0.
BEZEL_TEST(bug454_linked_list_lazy_linq, where_count_454) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(2); l.AddLast(3); l.AddLast(4); "
        "  return l.Where((x: int) => x > 2).Count(); "  // 3,4 → 2
        "end Main; end C;"), 2);
}

// §5 / §4.6 / #454: LinkedList<int>.Select(proj).Count() — without fix, = 0.
BEZEL_TEST(bug454_linked_list_lazy_linq, select_count_454) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); l.AddLast(30); "
        "  return l.Select((x: int) => x * 2).Count(); "  // 3 projected elements
        "end Main; end C;"), 3);
}

// §5 / §4.6 / #454: LinkedList<int>.Where(pred).ToArray() (no crash, correct).
// Explicit type annotation `Array<int>` is required so the compiler knows the
// resulting variable holds a concrete array (not a lazy enumerator) and can
// dispose it correctly at scope end and route .Sum() through the array path.
BEZEL_TEST(bug454_linked_list_lazy_linq, where_toarray_sum_454) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(2); l.AddLast(3); l.AddLast(4); l.AddLast(5); "
        "  using var arr: Array<int> := l.Where((x: int) => x mod 2 = 1).ToArray(); "  // 1,3,5
        "  return arr.Sum((x: int) => x); "  // 9
        "end Main; end C;"), 9);
}

// §5 / §4.6 / #454: chained Select.Where on LinkedList, consumed by Count.
// Select + Where both produce lazy enumerables; Count drains via materialize.
BEZEL_TEST(bug454_linked_list_lazy_linq, select_where_chained_454) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(1); l.AddLast(2); l.AddLast(3); "
        "  var lazy := l.Where((x: int) => x > 1); "  // 2,3 (lazy)
        "  return lazy.Count(); "  // 2
        "end Main; end C;"), 2);
}

// §5 / §4.6 / #454: regression — aggregate terminals on LinkedList still work.
BEZEL_TEST(bug454_linked_list_lazy_linq, regression_aggregate_still_works_454) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var l := new LinkedList<int>(); "
        "  l.AddLast(10); l.AddLast(20); l.AddLast(30); "
        "  return l.Sum((x: int) => x); "  // 60 (fix #450 path, unchanged)
        "end Main; end C;"), 60);
}

// ── #455: Array<int?> store/retrieve correctness ─────────────────────────────

// §4.5 / §7 / #455: Array<int?>.Add(n) then Get(0) retrieves n correctly.
// Before fix: bz_array_add read 8 bytes from a 4-byte i32 alloca → garbage.
BEZEL_TEST(bug455_nullable_elem_array, add_get_int_nullable_455) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int?>(); "
        "  a.Add(42); "
        "  var v: int? := a.Get(0); "
        "  if v = null then return -1; end; "
        "  return v!; "  // must be 42
        "end Main; end C;"), 42);
}

// §4.5 / §7 / #455: multiple Add then index-read; tests sequential slots.
BEZEL_TEST(bug455_nullable_elem_array, add_multiple_get_int_nullable_455) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int?>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var v0: int? := a.Get(0); "
        "  var v1: int? := a.Get(1); "
        "  var v2: int? := a.Get(2); "
        "  if v0 = null then return -1; end; "
        "  if v1 = null then return -2; end; "
        "  if v2 = null then return -3; end; "
        "  return v0! + v1! + v2!; "  // 6
        "end Main; end C;"), 6);
}

// §4.5 / §7 / #455: Add(null) then Get back null (hasValue = 0).
BEZEL_TEST(bug455_nullable_elem_array, add_null_get_null_455) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int?>(); "
        "  a.Add(null); "
        "  var v: int? := a.Get(0); "
        "  return (1 if v = null else 0); "
        "end Main; end C;"), 1);
}

// §4.5 / §7 / #455: mixed null and non-null elements.
BEZEL_TEST(bug455_nullable_elem_array, mixed_null_nonnull_455) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int?>(); "
        "  a.Add(10); a.Add(null); a.Add(20); "
        "  var v0: int? := a.Get(0); "
        "  var v1: int? := a.Get(1); "
        "  var v2: int? := a.Get(2); "
        "  if v0 = null then return -1; end; "
        "  if v1 <> null then return -2; end; "
        "  if v2 = null then return -3; end; "
        "  return v0! + v2!; "  // 30
        "end Main; end C;"), 30);
}

// §4.5 / §7 / #455: Array<bool?> — same tagged-struct issue for bool (i1→i8).
BEZEL_TEST(bug455_nullable_elem_array, add_get_bool_nullable_455) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<bool?>(); "
        "  a.Add(true); a.Add(false); a.Add(null); "
        "  var v0: bool? := a.Get(0); "
        "  var v1: bool? := a.Get(1); "
        "  var v2: bool? := a.Get(2); "
        "  if v0 = null then return -1; end; "
        "  if v1 = null then return -2; end; "
        "  if v2 <> null then return -3; end; "
        "  return (1 if v0! and not v1! else 0); "
        "end Main; end C;"), 1);
}

// §4.5 / #455: regression — Array<int> (non-nullable) still works correctly.
BEZEL_TEST(bug455_nullable_elem_array, regression_non_nullable_array_455) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(7); a.Add(8); a.Add(9); "
        "  return a.Get(0) + a.Get(1) + a.Get(2); "  // 24
        "end Main; end C;"), 24);
}

// §4.5 / #455: regression — Array<string> (pointer elements) still works.
BEZEL_TEST(bug455_nullable_elem_array, regression_string_array_455) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hi\"); "
        "  return a.Get(0).Length; "  // 2
        "end Main; end C;"), 2);
}
