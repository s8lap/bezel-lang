// Codegen — record/dict-backed Set: the set-algebra and LINQ cases #351/#381
// deferred (issue #389). stdlib §4.10 Set<T>/OwningSet<T>, §5 LINQ.
//
// What #351 already landed (collection_keys_test.cpp): record-Set Clear, and
// UnionWith/ExceptWith/IsSubsetOf/IsSupersetOf when `other` is a same-typed
// dict-backed Set. This file covers the remaining three items of #389:
//
//   1. IntersectWith — §4.10 Set<T>: "IntersectWith(other: IEnumerable<T>)".
//      Was deferred because it mutates self while it would have iterated self;
//      implemented snapshot-then-rebuild (snapshot `other`'s elements, then
//      remove from self every key not in that snapshot — no mutate-while-iterate
//      hazard). The dict-backed Set holds value-semantic elements (records,
//      primitives, enums, strings — genericBackingType), never disposable class
//      instances (OwningSet<T> where T: class is array-backed and not wired
//      here), so there is no element disposal on the dict-backed path.
//
//   2. Set-algebra where `other` is a NON-Set IEnumerable<T> (e.g. Array<T>):
//      §4.10 takes `other: IEnumerable<T>` for Union/Intersect/Except/IsSubsetOf/
//      IsSupersetOf. The previous path only handled a same-typed dict-backed Set
//      `other`; the general path enumerates `other` (the lazy enumerator, #326)
//      into a key snapshot and folds into/against self.
//
//   3. LINQ over a record-Set — §4.10: "Set<T>: ICollection<T>", and
//      ICollection<T>: IReadOnlyCollection<T>: IEnumerable<T> (§4.3), so a Set
//      is an IEnumerable<T> and feeds the lazy LINQ pipeline (§5). The Set→
//      enumerator adapter snapshots the dict's keys and iterates them.
//
// Each test is load-bearing: before the fix IntersectWith was a no-op (returned
// self unchanged), array-`other` set-algebra was a no-op (deferred → fell
// through), and LINQ over a dict-backed Set wrapped the dict pointer as an array
// and produced wrong results.

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
// Item 1 — IntersectWith (record-Set, same-typed Set `other`)
// ===========================================================================

// §4.10: Set<record>.IntersectWith(other) keeps only elements present in BOTH.
// {(1,2),(3,4),(5,6)} ∩ {(3,4),(5,6),(7,8)} = {(3,4),(5,6)} → Count 2; (1,2)
// gone, (3,4)/(5,6) kept. Before the fix IntersectWith was a no-op → 3.
BEZEL_TEST(set_algebra_389, set_record_intersectwith_keeps_common_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Set<P>(); using var b := new Set<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); a.Add(new P(5, 6)); "
        "  b.Add(new P(3, 4)); b.Add(new P(5, 6)); b.Add(new P(7, 8)); "
        "  a.IntersectWith(b); "
        "  return a.Count * 100 + int(a.Contains(new P(3, 4))) * 10 "
        "         + int(not a.Contains(new P(1, 2))); "   // 2*100 + 1*10 + 1 = 211
        "end Main; end C;"), 211);
}

// §4.10: IntersectWith with a DISJOINT other empties self. {(1,2)} ∩ {(9,9)} = {}
// → Count 0. (No mutate-while-iterate crash: self is snapshotted first.)
BEZEL_TEST(set_algebra_389, set_record_intersectwith_disjoint_empties_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Set<P>(); using var b := new Set<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  b.Add(new P(9, 9)); "
        "  a.IntersectWith(b); "
        "  return a.Count; "                              // 0
        "end Main; end C;"), 0);
}

// §4.10: IntersectWith with a SUPERSET other leaves self unchanged.
// {(1,2),(3,4)} ∩ {(1,2),(3,4),(5,6)} = {(1,2),(3,4)} → Count 2, both present.
BEZEL_TEST(set_algebra_389, set_record_intersectwith_superset_unchanged_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Set<P>(); using var b := new Set<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  b.Add(new P(1, 2)); b.Add(new P(3, 4)); b.Add(new P(5, 6)); "
        "  a.IntersectWith(b); "
        "  return a.Count * 10 + int(a.Contains(new P(1, 2)) "
        "         and a.Contains(new P(3, 4))); "         // 2*10 + 1 = 21
        "end Main; end C;"), 21);
}

// §4.10: IntersectWith over a primitive (int) dict-backed Set — the generic
// backing also covers int elements (D3/D8). {1,2,3,4} ∩ {2,4,6} = {2,4}.
BEZEL_TEST(set_algebra_389, set_int_intersectwith_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Set<int>(); using var b := new Set<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  b.Add(2); b.Add(4); b.Add(6); "
        "  a.IntersectWith(b); "
        "  return a.Count * 100 + int(a.Contains(2)) * 10 + int(not a.Contains(1)); "
        "end Main; end C;"), 211);                        // 2*100 + 10 + 1
}

// ===========================================================================
// Item 2 — set-algebra where `other` is a non-Set IEnumerable<T> (Array<T>)
// ===========================================================================

// §4.10: Set<record>.UnionWith(other: IEnumerable<T>) where other is Array<P>.
// {(1,2)} ∪ [(3,4),(1,2),(5,6)] = {(1,2),(3,4),(5,6)} → Count 3 (the duplicate
// (1,2) is collapsed). Before the fix array-`other` deferred → no-op (Count 1).
BEZEL_TEST(set_algebra_389, set_record_unionwith_array_other_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<P>(); "
        "  s.Add(new P(1, 2)); "
        "  using var a := new Array<P>(); "
        "  a.Add(new P(3, 4)); a.Add(new P(1, 2)); a.Add(new P(5, 6)); "
        "  s.UnionWith(a); "
        "  return s.Count; "                              // 3
        "end Main; end C;"), 3);
}

// §4.10: Set<record>.ExceptWith(other: IEnumerable<T>) where other is Array<P>.
// {(1,2),(3,4),(5,6)} \ [(3,4),(7,8)] = {(1,2),(5,6)} → Count 2; (3,4) gone.
BEZEL_TEST(set_algebra_389, set_record_exceptwith_array_other_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<P>(); "
        "  s.Add(new P(1, 2)); s.Add(new P(3, 4)); s.Add(new P(5, 6)); "
        "  using var a := new Array<P>(); "
        "  a.Add(new P(3, 4)); a.Add(new P(7, 8)); "
        "  s.ExceptWith(a); "
        "  return s.Count * 10 + int(not s.Contains(new P(3, 4))); "  // 2*10 + 1 = 21
        "end Main; end C;"), 21);
}

// §4.10: Set<record>.IntersectWith(other: IEnumerable<T>) where other is
// Array<P>. {(1,2),(3,4),(5,6)} ∩ [(3,4),(5,6),(7,8)] = {(3,4),(5,6)} → Count 2.
BEZEL_TEST(set_algebra_389, set_record_intersectwith_array_other_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<P>(); "
        "  s.Add(new P(1, 2)); s.Add(new P(3, 4)); s.Add(new P(5, 6)); "
        "  using var a := new Array<P>(); "
        "  a.Add(new P(3, 4)); a.Add(new P(5, 6)); a.Add(new P(7, 8)); "
        "  s.IntersectWith(a); "
        "  return s.Count * 10 + int(not s.Contains(new P(1, 2))); "  // 2*10 + 1 = 21
        "end Main; end C;"), 21);
}

// §4.10: Set<record>.IsSubsetOf(other: IEnumerable<T>) where other is Array<P>.
// {(1,2)} ⊆ [(1,2),(3,4)] → true; {(1,2),(9,9)} ⊄ [(1,2),(3,4)] → false.
BEZEL_TEST(set_algebra_389, set_record_issubsetof_array_other_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Set<P>(); using var c := new Set<P>(); "
        "  a.Add(new P(1, 2)); "
        "  c.Add(new P(1, 2)); c.Add(new P(9, 9)); "
        "  using var o := new Array<P>(); "
        "  o.Add(new P(1, 2)); o.Add(new P(3, 4)); "
        "  return int(a.IsSubsetOf(o)) * 10 + int(c.IsSubsetOf(o)); "  // 1*10 + 0 = 10
        "end Main; end C;"), 10);
}

// §4.10: Set<record>.IsSupersetOf(other: IEnumerable<T>) where other is
// Array<P>. {(1,2),(3,4)} ⊇ [(1,2)] → true; ⊉ [(7,7)] → false.
BEZEL_TEST(set_algebra_389, set_record_issupersetof_array_other_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Set<P>(); "
        "  a.Add(new P(1, 2)); a.Add(new P(3, 4)); "
        "  using var o1 := new Array<P>(); o1.Add(new P(1, 2)); "
        "  using var o2 := new Array<P>(); o2.Add(new P(7, 7)); "
        "  return int(a.IsSupersetOf(o1)) * 10 + int(a.IsSupersetOf(o2)); "  // 10
        "end Main; end C;"), 10);
}

// §4.10: UnionWith with an Array<int> other over an int-backed Set.
// {1,2} ∪ [2,3,4] = {1,2,3,4} → Count 4.
BEZEL_TEST(set_algebra_389, set_int_unionwith_array_other_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); s.Add(1); s.Add(2); "
        "  using var a := new Array<int>(); a.Add(2); a.Add(3); a.Add(4); "
        "  s.UnionWith(a); "
        "  return s.Count; "                              // 4
        "end Main; end C;"), 4);
}

// ===========================================================================
// Item 3 — LINQ over a record-Set (Set is IEnumerable<T>, §4.10 / §5)
// ===========================================================================

// §5: Set<record>.Where(pred).ToArray() — the Set feeds the lazy pipeline.
// {(1,2),(3,4),(5,6)} filtered to x > 2 yields {(3,4),(5,6)} → 2 elements.
BEZEL_TEST(set_algebra_389, set_record_linq_where_count_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<P>(); "
        "  s.Add(new P(1, 2)); s.Add(new P(3, 4)); s.Add(new P(5, 6)); "
        "  using var r: Array<P> := s.Where((p: P) => p.x > 2).ToArray(); "
        "  return r.Count; "                              // 2
        "end Main; end C;"), 2);
}

// §5: Set<record>.Select(sel).ToArray() — project each element to an int and
// sum. {(1,2),(3,4),(5,6)} → x sum = 1+3+5 = 9 (order-independent sum).
BEZEL_TEST(set_algebra_389, set_record_linq_select_sum_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<P>(); "
        "  s.Add(new P(1, 2)); s.Add(new P(3, 4)); s.Add(new P(5, 6)); "
        "  using var xs: Array<int> := s.Select((p: P) => p.x).ToArray(); "
        "  var total := 0; "
        "  for var i in 0..<xs.Count do total := total + xs.Get(i); end; "
        "  return total; "                                // 9
        "end Main; end C;"), 9);
}

// §5: LINQ ToArray() over a record-Set materializes EVERY element exactly once.
// {(1,2),(3,4),(5,6)} → ToArray has 3 elements; the dedup already happened in
// the Set, so no duplicates appear.
BEZEL_TEST(set_algebra_389, set_record_linq_toarray_all_elements_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var s := new Set<P>(); "
        "  s.Add(new P(1, 2)); s.Add(new P(3, 4)); s.Add(new P(5, 6)); "
        "  s.Add(new P(3, 4)); "                          // dup → collapsed
        "  using var r: Array<P> := s.Select((p: P) => p).ToArray(); "
        "  return r.Count; "                              // 3
        "end Main; end C;"), 3);
}

// §5: LINQ over an int-backed Set. {10,20,30} → Where(x > 15) → {20,30}, sum 50.
BEZEL_TEST(set_algebra_389, set_int_linq_where_select_sum_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); s.Add(10); s.Add(20); s.Add(30); "
        "  using var xs: Array<int> := s.Where((x: int) => x > 15).ToArray(); "
        "  var total := 0; "
        "  for var i in 0..<xs.Count do total := total + xs.Get(i); end; "
        "  return total; "                                // 50
        "end Main; end C;"), 50);
}
