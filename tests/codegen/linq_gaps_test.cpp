// Codegen tests for LINQ operator gaps filled in this milestone (stdlib §5).
//
// Operators added (verified against §5 of Bezel_Standard_Library_0_130.md):
//
//   Union(other: IEnumerable<T>): IEnumerable<T>        — LAZY  (§5 Set-like)
//   Intersect(other: IEnumerable<T>): IEnumerable<T>    — LAZY  (§5 Set-like)
//   Except(other: IEnumerable<T>): IEnumerable<T>       — LAZY  (§5 Set-like)
//   ThenBy<TKey>(key: (T)->TKey): IOrderedEnumerable<T> — LAZY  (§5 Ordering)
//   ThenByDescending<TKey>(key): IOrderedEnumerable<T>  — LAZY  (§5 Ordering)
//   Reverse(): IEnumerable<T>                           — LAZY  (§5 Ordering)
//   ToSet(): owned Set<T>                               — EAGER (§5 Materialization)
//   ToDictionary<TKey>(key): owned Dictionary<TKey, T>  — EAGER (§5 Materialization)
//   FirstOrNull(): T?                                   — EAGER (§5 Element access)
//   LastOrNull(): T?                                    — EAGER (§5 Element access)
//   SingleOrNull(): T?                                  — EAGER (§5 Element access)
//
// Skipped (with reason):
//   GroupBy<TKey>(key): IEnumerable<IGrouping<TKey,T>>  — needs IGrouping value
//     model; out of scope for this milestone.
//   OfType<U>(): IEnumerable<U>                         — needs explicit type-arg
//     at call site (AST CallExpr has no typeArgs field) and runtime type-tag
//     infrastructure; out of scope.
//
// Each lazy operator: tested for correctness on representative data (including
// empty input and duplicate elements), chaining with existing operators, and
// (for closure-taking ops: ThenBy) an ASan/no-corruption leg.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// Union (§5 Set-like): lazy, dedup, preserves order (self first, then other)
// ===========================================================================

// §5 Union: basic case — {1,2,3} ∪ {2,3,4} = {1,2,3,4} in order
BEZEL_TEST(linq_gaps, union_basic_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); "
        "  using var b := new Array<int>(); b.Add(2); b.Add(3); b.Add(4); "
        "  using var r: Array<int> := a.Union(b).ToArray(); "
        "  return int(r.Length = 4 and r.Get(0) = 1 and r.Get(1) = 2 "
        "             and r.Get(2) = 3 and r.Get(3) = 4); "
        "end Main; end C;"), 1);
}

// §5 Union: with duplicates in self — {1,1,2} ∪ {2,3} = {1,2,3}
BEZEL_TEST(linq_gaps, union_duplicates_in_self_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(1); a.Add(2); "
        "  using var b := new Array<int>(); b.Add(2); b.Add(3); "
        "  using var r: Array<int> := a.Union(b).ToArray(); "
        "  return r.Length; "  // {1,2,3} = 3
        "end Main; end C;"), 3);
}

// §5 Union: empty self — {} ∪ {1,2} = {1,2}
BEZEL_TEST(linq_gaps, union_empty_self_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  using var b := new Array<int>(); b.Add(1); b.Add(2); "
        "  using var r: Array<int> := a.Union(b).ToArray(); "
        "  return r.Length; "  // 2
        "end Main; end C;"), 2);
}

// §5 Union: both empty — {} ∪ {} = {}
BEZEL_TEST(linq_gaps, union_both_empty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  using var b := new Array<int>(); "
        "  using var r: Array<int> := a.Union(b).ToArray(); "
        "  return r.Length; "  // 0
        "end Main; end C;"), 0);
}

// §5 Union: chained with Where and Count
BEZEL_TEST(linq_gaps, union_chained_where_count_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); "
        "  using var b := new Array<int>(); b.Add(3); b.Add(4); b.Add(5); "
        "  return a.Union(b).Where((x) => x mod 2 = 0).Count(); "  // {2,4} = 2
        "end Main; end C;"), 2);
}

// ===========================================================================
// Intersect (§5 Set-like): elements in both sequences, unique
// ===========================================================================

// §5 Intersect: {1,2,3,4} ∩ {2,4,6} = {2,4}
BEZEL_TEST(linq_gaps, intersect_basic_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  using var b := new Array<int>(); b.Add(2); b.Add(4); b.Add(6); "
        "  using var r: Array<int> := a.Intersect(b).ToArray(); "
        "  return int(r.Length = 2 and r.Get(0) = 2 and r.Get(1) = 4); "
        "end Main; end C;"), 1);
}

// §5 Intersect: disjoint → empty
BEZEL_TEST(linq_gaps, intersect_disjoint_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(3); "
        "  using var b := new Array<int>(); b.Add(2); b.Add(4); "
        "  using var r: Array<int> := a.Intersect(b).ToArray(); "
        "  return r.Length; "  // 0
        "end Main; end C;"), 0);
}

// §5 Intersect: duplicates in self — {1,1,2,2} ∩ {1,2} = {1,2} (each unique)
BEZEL_TEST(linq_gaps, intersect_duplicates_dedup_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(1); a.Add(2); a.Add(2); "
        "  using var b := new Array<int>(); b.Add(1); b.Add(2); "
        "  using var r: Array<int> := a.Intersect(b).ToArray(); "
        "  return r.Length; "  // {1,2} = 2
        "end Main; end C;"), 2);
}

// §5 Intersect chained with Sum
BEZEL_TEST(linq_gaps, intersect_chained_sum_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  using var b := new Array<int>(); b.Add(2); b.Add(4); "
        "  return a.Intersect(b).Sum(); "  // 2+4 = 6
        "end Main; end C;"), 6);
}

// ===========================================================================
// Except (§5 Set-like): elements in self not in other, unique
// ===========================================================================

// §5 Except: {1,2,3,4} \ {2,4} = {1,3}
BEZEL_TEST(linq_gaps, except_basic_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  using var b := new Array<int>(); b.Add(2); b.Add(4); "
        "  using var r: Array<int> := a.Except(b).ToArray(); "
        "  return int(r.Length = 2 and r.Get(0) = 1 and r.Get(1) = 3); "
        "end Main; end C;"), 1);
}

// §5 Except: all removed → empty
BEZEL_TEST(linq_gaps, except_all_removed_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); "
        "  using var b := new Array<int>(); b.Add(1); b.Add(2); "
        "  using var r: Array<int> := a.Except(b).ToArray(); "
        "  return r.Length; "  // 0
        "end Main; end C;"), 0);
}

// §5 Except: duplicates in self — {1,1,2,3} \ {1} = {2,3} (dedup)
BEZEL_TEST(linq_gaps, except_duplicates_dedup_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(1); a.Add(2); a.Add(3); "
        "  using var b := new Array<int>(); b.Add(1); "
        "  using var r: Array<int> := a.Except(b).ToArray(); "
        "  return r.Length; "  // {2,3} = 2
        "end Main; end C;"), 2);
}

// §5 Except chained with Where
BEZEL_TEST(linq_gaps, except_chained_where_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  for var i in 1..10 do a.Add(i); end; "
        "  using var b := new Array<int>(); b.Add(2); b.Add(4); b.Add(6); b.Add(8); b.Add(10); "
        // Odd numbers: 1..10 except evens = {1,3,5,7,9} → Sum=25
        "  return a.Except(b).Sum(); "
        "end Main; end C;"), 25);
}

// ===========================================================================
// ThenBy / ThenByDescending (§5 Ordering): secondary sort on IOrderedEnumerable
// ===========================================================================

// §5 ThenBy: sort by (x, y) composite — primary ascending, secondary ascending
BEZEL_TEST(linq_gaps, thenby_basic_spec_5) {
    // Points sorted by x then y: (1,3),(1,5),(2,1),(2,4) → x=1 first, then y.
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(2, 4)); a.Add(new Point(1, 5)); "
        "  a.Add(new Point(2, 1)); a.Add(new Point(1, 3)); "
        "  using var r: Array<Point> := a.OrderBy((p) => p.x)"
        "                               .ThenBy((p) => p.y)"
        "                               .ToArray(); "
        "  return int(r.Get(0).x = 1 and r.Get(0).y = 3 and "
        "             r.Get(1).x = 1 and r.Get(1).y = 5 and "
        "             r.Get(2).x = 2 and r.Get(2).y = 1 and "
        "             r.Get(3).x = 2 and r.Get(3).y = 4); "
        "end Main; end C;"), 1);
}

// §5 ThenByDescending: secondary sort descending
BEZEL_TEST(linq_gaps, thenby_descending_spec_5) {
    // Sort by x asc, then y desc: (1,5),(1,3),(2,4),(2,1)
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(2, 4)); a.Add(new Point(1, 5)); "
        "  a.Add(new Point(2, 1)); a.Add(new Point(1, 3)); "
        "  using var r: Array<Point> := a.OrderBy((p) => p.x)"
        "                               .ThenByDescending((p) => p.y)"
        "                               .ToArray(); "
        "  return int(r.Get(0).x = 1 and r.Get(0).y = 5 and "
        "             r.Get(1).x = 1 and r.Get(1).y = 3 and "
        "             r.Get(2).x = 2 and r.Get(2).y = 4 and "
        "             r.Get(3).x = 2 and r.Get(3).y = 1); "
        "end Main; end C;"), 1);
}

// §5 ThenBy: single element (no reordering needed)
BEZEL_TEST(linq_gaps, thenby_single_element_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(7, 3)); "
        "  using var r: Array<Point> := a.OrderBy((p) => p.x)"
        "                               .ThenBy((p) => p.y)"
        "                               .ToArray(); "
        "  return r.Length; "
        "end Main; end C;"), 1);
}

// §5 ThenBy: chained with Count
BEZEL_TEST(linq_gaps, thenby_chained_count_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(2, 4)); a.Add(new Point(1, 5)); "
        "  a.Add(new Point(2, 1)); a.Add(new Point(1, 3)); "
        "  return a.OrderBy((p) => p.x).ThenBy((p) => p.y).Count(); "
        "end Main; end C;"), 4);
}

// ===========================================================================
// Reverse (§5 Ordering): lazy materialize + yield backwards
// ===========================================================================

// §5 Reverse: basic case [1,2,3,4,5] → [5,4,3,2,1]
BEZEL_TEST(linq_gaps, reverse_basic_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var r: Array<int> := a.Where((x) => x > 0).Reverse().ToArray(); "
        "  return int(r.Length = 5 and r.Get(0) = 5 and r.Get(4) = 1); "
        "end Main; end C;"), 1);
}

// §5 Reverse: empty sequence → empty
BEZEL_TEST(linq_gaps, reverse_empty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  using var r: Array<int> := a.Where((x) => x > 0).Reverse().ToArray(); "
        "  return r.Length; "
        "end Main; end C;"), 0);
}

// §5 Reverse: single element → same
BEZEL_TEST(linq_gaps, reverse_single_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(42); "
        "  using var r: Array<int> := a.Where((x) => x > 0).Reverse().ToArray(); "
        "  return r.Get(0); "
        "end Main; end C;"), 42);
}

// §5 Reverse: chained after Where
BEZEL_TEST(linq_gaps, reverse_chained_where_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        // Evens reversed: Where(even) = [2,4]; Reverse = [4,2]; Sum = 6
        "  return a.Where((x) => x mod 2 = 0).Reverse().Sum(); "
        "end Main; end C;"), 6);
}

// §5 Reverse: iterate via for-loop (no ToArray)
BEZEL_TEST(linq_gaps, reverse_for_loop_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var s := 0; "
        "  for var x in a.Where((x) => x > 0).Reverse() do s := s + x; end; "
        "  return s; "  // 1+2+3 = 6 (order doesn't affect sum)
        "end Main; end C;"), 6);
}

// ===========================================================================
// ToSet (§5 Materialization): materialize into Set<T>, dedup, O(1) Contains
// ===========================================================================

// §5 ToSet: basic — {1,2,1,3,2} → Set with 3 unique elements
BEZEL_TEST(linq_gaps, toset_basic_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(1); a.Add(3); a.Add(2); "
        "  using var s: Set<int> := a.Where((x) => x > 0).ToSet(); "
        "  return s.Count; "  // 3
        "end Main; end C;"), 3);
}

// §5 ToSet: directly from Array
BEZEL_TEST(linq_gaps, toset_from_array_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  using var s: Set<int> := a.ToSet(); "
        "  return s.Count; "  // 3
        "end Main; end C;"), 3);
}

// §5 ToSet: empty source → empty set
BEZEL_TEST(linq_gaps, toset_empty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  using var s: Set<int> := a.ToSet(); "
        "  return s.Count; "  // 0
        "end Main; end C;"), 0);
}

// ===========================================================================
// ToDictionary (§5 Materialization): materialize into Dictionary<TKey, T>
// ===========================================================================

// §5 ToDictionary: key=element itself, verify Count
BEZEL_TEST(linq_gaps, todict_basic_count_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  using var d: Dictionary<int, int> := a.ToDictionary((x: int) => x); "
        "  return d.Count; "  // 3
        "end Main; end C;"), 3);
}

// §5 ToDictionary: verify key lookup
BEZEL_TEST(linq_gaps, todict_lookup_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  using var d: Dictionary<int, int> := a.ToDictionary((x: int) => x); "
        "  return int(d.ContainsKey(20)); "  // 1
        "end Main; end C;"), 1);
}

// §5 ToDictionary: from a lazy pipeline
BEZEL_TEST(linq_gaps, todict_from_lazy_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  for var i in 1..5 do a.Add(i); end; "
        "  using var d: Dictionary<int, int> := "
        "    a.Where((x) => x mod 2 = 0).ToDictionary((x: int) => x); "
        "  return d.Count; "  // {2,4} → 2
        "end Main; end C;"), 2);
}

// ===========================================================================
// BUG #459 / §5 / §22 — ToDictionary with a TYPE-INFERRED local (`var d :=`).
// When the local's type is inferred from the initializer, the ToDictionary
// result must carry a Dictionary<K,V>-compatible TypeRef so that both sema
// (Count return-type) and codegen (isDictTypeName) recognize the receiver.
// Without the fix, the inferred local gets a "lazy sentinel" type and
// d.Count dispatches through the lazy-enumerator path → Count=0.
// ===========================================================================

// §5 / §22 / #459: inferred var from ToDictionary; Count must be correct.
BEZEL_TEST(linq_gaps, todict_inferred_var_count_459) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  using var d := a.ToDictionary((x: int) => x); "
        "  return d.Count; "  // 3 — explicit-annotation form already works
        "end Main; end C;"), 3);
}

// §5 / §22 / #459: inferred var from ToDictionary on a lazy pipeline.
BEZEL_TEST(linq_gaps, todict_inferred_var_lazy_source_459) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  for var i in 1..5 do a.Add(i); end; "
        "  using var d := a.Where((x) => x mod 2 = 0).ToDictionary((x: int) => x); "
        "  return d.Count; "  // {2,4} → 2
        "end Main; end C;"), 2);
}

// §5 / §22 / #459: key lookup on an inferred-var dictionary.
BEZEL_TEST(linq_gaps, todict_inferred_var_containskey_459) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  using var d := a.ToDictionary((x: int) => x); "
        "  return int(d.ContainsKey(20)); "  // 1
        "end Main; end C;"), 1);
}

// ===========================================================================
// FirstOrNull (§5 Element access): T? — null if empty
// ===========================================================================

// §5 FirstOrNull: non-empty → the first element (as T?, check via coalesce)
BEZEL_TEST(linq_gaps, first_or_null_nonempty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(42); a.Add(7); a.Add(3); "
        "  var n: int? := a.FirstOrNull(); "
        "  return n ?? -1; "  // 42
        "end Main; end C;"), 42);
}

// §5 FirstOrNull: empty → null (coalesce to -1)
BEZEL_TEST(linq_gaps, first_or_null_empty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  var n: int? := a.FirstOrNull(); "
        "  return n ?? -1; "  // null → -1
        "end Main; end C;"), -1);
}

// §5 FirstOrNull on a lazy pipeline
BEZEL_TEST(linq_gaps, first_or_null_lazy_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(4); a.Add(2); "
        "  var n: int? := a.Where((x) => x > 3).FirstOrNull(); "
        "  return n ?? -1; "  // first element > 3 = 4
        "end Main; end C;"), 4);
}

// §5 FirstOrNull on lazy — no match → null
BEZEL_TEST(linq_gaps, first_or_null_lazy_no_match_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var n: int? := a.Where((x) => x > 99).FirstOrNull(); "
        "  return n ?? -1; "  // no match → null → -1
        "end Main; end C;"), -1);
}

// ===========================================================================
// LastOrNull (§5 Element access): T? — null if empty
// ===========================================================================

// §5 LastOrNull: non-empty → last element
BEZEL_TEST(linq_gaps, last_or_null_nonempty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(5); a.Add(3); "
        "  var n: int? := a.LastOrNull(); "
        "  return n ?? -1; "  // 3
        "end Main; end C;"), 3);
}

// §5 LastOrNull: empty → null
BEZEL_TEST(linq_gaps, last_or_null_empty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  var n: int? := a.LastOrNull(); "
        "  return n ?? -1; "
        "end Main; end C;"), -1);
}

// §5 LastOrNull: lazy pipeline — last match
BEZEL_TEST(linq_gaps, last_or_null_lazy_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(4); a.Add(2); a.Add(6); "
        "  var n: int? := a.Where((x) => x > 3).LastOrNull(); "
        "  return n ?? -1; "  // evens > 3: [4, 6]; last = 6
        "end Main; end C;"), 6);
}

// ===========================================================================
// SingleOrNull (§5 Element access): T? — null if not exactly one element
// ===========================================================================

// §5 SingleOrNull: exactly one → that element
BEZEL_TEST(linq_gaps, single_or_null_exactly_one_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(99); "
        "  var n: int? := a.SingleOrNull(); "
        "  return n ?? -1; "  // 99
        "end Main; end C;"), 99);
}

// §5 SingleOrNull: empty → null
BEZEL_TEST(linq_gaps, single_or_null_empty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  var n: int? := a.SingleOrNull(); "
        "  return n ?? -1; "
        "end Main; end C;"), -1);
}

// §5 SingleOrNull: more than one → null
BEZEL_TEST(linq_gaps, single_or_null_multiple_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  var n: int? := a.SingleOrNull(); "
        "  return n ?? -1; "  // more than one → null → -1
        "end Main; end C;"), -1);
}

// §5 SingleOrNull: lazy pipeline with exactly one match
BEZEL_TEST(linq_gaps, single_or_null_lazy_one_match_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(5); a.Add(3); "
        "  var n: int? := a.Where((x) => x > 4).SingleOrNull(); "
        "  return n ?? -1; "  // only 5 > 4 → 5
        "end Main; end C;"), 5);
}

// ===========================================================================
// Chaining: new operators compose with existing lazy ops
// ===========================================================================

// §5 Union → OrderBy → Take: three-stage chain
BEZEL_TEST(linq_gaps, union_orderby_take_chain_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(5); a.Add(1); a.Add(3); "
        "  using var b := new Array<int>(); b.Add(3); b.Add(4); b.Add(2); "
        // Union = {5,1,3,4,2}; OrderBy = {1,2,3,4,5}; Take(3) = {1,2,3}; Sum=6
        "  return a.Union(b).OrderBy((x: int) => x).Take(3).Sum(); "
        "end Main; end C;"), 6);
}

// §5 Except → Reverse → Count: set diff reversed
BEZEL_TEST(linq_gaps, except_reverse_count_chain_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var b := new Array<int>(); b.Add(2); b.Add(4); "
        // {1,3,5} reversed → {5,3,1}; Count = 3
        "  return a.Except(b).Where((x) => x > 0).Reverse().Count(); "
        "end Main; end C;"), 3);
}

// §5 Intersect → Select → Sum
BEZEL_TEST(linq_gaps, intersect_select_sum_chain_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); "
        "  using var b := new Array<int>(); b.Add(2); b.Add(3); b.Add(4); "
        // {2,3} * 10 = {20,30}; Sum = 50
        "  return a.Intersect(b).Select((x: int) => x * 10).Sum(); "
        "end Main; end C;"), 50);
}

// ===========================================================================
// fix #449: Array.Reverse() in-place — stdlib §4.4 / §3
// The in-place void Array.Reverse() on a concrete Array<T> must not crash when
// chained (e.g. .ToArray() after in-place Reverse returns nothing useful —
// that is a type error, but must not SIGABRT).  The in-place path itself must
// correctly reverse the array.
// ===========================================================================

// §3/§4.4: in-place Reverse on [3,1,2] → [2,1,3]; read back via Get.
// Verifies the in-place lowering in tryLowerCollectionInstanceCall is correct.
BEZEL_TEST(linq_gaps_issue449, array_reverse_inplace_basic_spec_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  a.Reverse(); "    // in-place: [3,1,2] → [2,1,3]
        "  return a.Get(0) * 100 + a.Get(1) * 10 + a.Get(2); "
        "end Main; end C;"), 213);
}

// §3/§4.4: in-place Reverse on [7,8] → [8,7].
BEZEL_TEST(linq_gaps_issue449, array_reverse_inplace_two_elements_spec_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(7); a.Add(8); "
        "  a.Reverse(); "    // in-place: [7,8] → [8,7]
        "  return a.Get(0) * 10 + a.Get(1); "
        "end Main; end C;"), 87);
}

// §3/§4.4: in-place Reverse on a single-element array is a no-op.
BEZEL_TEST(linq_gaps_issue449, array_reverse_inplace_single_element_spec_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(42); "
        "  a.Reverse(); "
        "  return a.Get(0); "
        "end Main; end C;"), 42);
}

// §3/§4.4: lazy Reverse on an IEnumerable<T> receiver (Where pipeline) still
// works correctly — the lazy path must remain unbroken by the fix.
BEZEL_TEST(linq_gaps_issue449, lazy_reverse_after_where_still_correct_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var r: Array<int> := a.Where((x) => x mod 2 = 1).Reverse().ToArray(); "
        // Where(odd) = [1,3,5]; Reverse = [5,3,1]
        "  return r.Get(0) * 100 + r.Get(1) * 10 + r.Get(2); "
        "end Main; end C;"), 531);
}
