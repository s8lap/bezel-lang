// Codegen — OwningArray<T>.Remove(item) / Insert / and the Array<T> equivalents
// (issue #381, stdlib §4.5).
//
// stdlib §4.5: "`OwningArray<T>` owns each element. `Add`, `Insert`, and the
// indexer setter take ownership; `RemoveAt`, `Remove`, `Clear`, and the array's
// own `Dispose()` dispose the affected elements." And: "`Remove(item: T): bool;
// // borrow-taking query; disposes the matched element". And the table:
// "`Insert(index: int, owned item: T)`".
//
// #362 already covered Dispose/Clear/RemoveAt; this file covers Remove(item)
// (find-and-remove-and-dispose) and Insert(index, item) (shift-and-store).
//
// Disposal is made observable through a static counter the element's destructor
// bumps; the counter is returned (often encoded) as the exit code. Without the
// fix Remove(item) was a no-op (returned nullptr in codegen) and Insert did
// nothing — so these are load-bearing.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// A resource class R (destructor bumps R._n) wrapped in a holder C with the
// given Main body, returning the encoded counter.
int runResource(const std::string& mainBody) {
    return run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;");
}

}  // namespace

// stdlib §4.5: OwningArray<T>.Remove(item) finds the matching element, DISPOSES
// it (exactly once), and removes its slot. The matched element is found via
// a.Get(1) (a borrow of the stored slot). Encoded: <count right after Remove>
// *10 + <count after the array's own scope-end Dispose>. Remove disposes 1 of
// 3; scope-end disposes the other 2 → 13 (and the survivors are NOT
// double-disposed). Before the fix Remove was a no-op → 03.
BEZEL_TEST(owned_coll_ops_4_5, owned_array_remove_item_disposes_only_matched) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterRemove := -1; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  var removed := a.Remove(a.Get(1)); "
        "  afterRemove := R._n; "
        "end; "
        "return afterRemove * 10 + R._n;"), 13);
}

// stdlib §4.5: Remove(item): bool returns true when an element matched (was
// found and removed). a.Get(0) is the stored reference, so it matches.
BEZEL_TEST(owned_coll_ops_4_5, owned_array_remove_item_returns_true_on_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); a.Add(move (new R())); "
        "  var hit := a.Remove(a.Get(0)); "
        "  return int(hit) * 100 + a.Count; "          // 1*100 + 1 = 101
        "end Main; end C;"), 101);
}

// stdlib §4.5: a borrowing Array<T>.Remove(item) removes the slot but does NOT
// dispose the element (borrows are owned elsewhere). Matched by REFERENCE
// identity (§25.2 default Equals for classes). r2 is the second element; after
// Remove the count is 2 and no destructor has run (the three `using var`
// resources dispose only at Main's scope end). Encoded: int(removed)*100 +
// Count = 102, with R._n still 0 at the return point.
BEZEL_TEST(owned_coll_ops_4_5, borrowing_array_remove_item_no_dispose) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var a := new Array<R>(); "
        "  using var r1 := new R(); using var r2 := new R(); using var r3 := new R(); "
        "  a.Add(r1); a.Add(r2); a.Add(r3); "
        "  var removed := a.Remove(r2); "
        "  return int(removed) * 100 + a.Count * 10 + R._n; " // 1*100+2*10+0 = 120
        "end Main; end C;"), 120);
}

// stdlib §4.5: Remove(item) returns false (and disposes nothing) when no element
// matches. A fresh, never-stored instance is reference-distinct from every slot.
BEZEL_TEST(owned_coll_ops_4_5, owned_array_remove_item_miss_returns_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); a.Add(move (new R())); "
        "  using var probe := new R(); "
        "  var hit := a.Remove(probe); "                 // probe is not stored
        "  return int(hit) * 100 + a.Count * 10 + R._n; " // 0*100+2*10+0 = 20
        "end Main; end C;"), 20);
}

// stdlib §4.5: OwningArray<T>.Insert(index, item) inserts at the index, shifting
// later elements up; Count grows by one. Insert does not dispose (a fresh slot
// is created, not a replacement), so no destructor runs at the Insert point.
BEZEL_TEST(owned_coll_ops_4_5, owned_array_insert_grows_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); a.Add(move (new R())); "
        "  a.Insert(1, move (new R())); "
        "  return a.Count * 10 + R._n; "                 // count 3, nothing disposed → 30
        "end Main; end C;"), 30);
}

// stdlib §4.5 / §3: Array<int>.Insert(index, item) shifts the existing element
// at `index` (and all after) up by one and stores the new element there. After
// Insert(1, 2) on [1, 3] the array is [1, 2, 3].
BEZEL_TEST(owned_coll_ops_4_5, array_int_insert_shifts_up) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(3); "
        "  a.Insert(1, 2); "
        "  return a.Count * 100 + a.Get(1) * 10 + a.Get(2); " // 3*100 + 2*10 + 3 = 323
        "end Main; end C;"), 323);
}

// stdlib §4.5 / §3: Insert at index 0 prepends; Insert at Count appends.
BEZEL_TEST(owned_coll_ops_4_5, array_int_insert_at_ends) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(5); "
        "  a.Insert(0, 1); "          // prepend → [1, 5]
        "  a.Insert(2, 9); "          // append at Count → [1, 5, 9]
        "  return a.Get(0) * 100 + a.Get(1) * 10 + a.Get(2); "  // 1*100 + 5*10 + 9 = 159
        "end Main; end C;"), 159);
}

// ===========================================================================
// #1200 — Array<T>.RemoveAt(i) and Insert(i, v) must bounds-check i and raise
// IndexOutOfRangeException instead of silently clamping/no-oping.
// stdlib §4.5: both route through BoundsCheck.
// RemoveAt valid range: 0 <= i < Count.  Insert valid range: 0 <= i <= Count.
// ===========================================================================

// §4.5 (#1200): RemoveAt(10) on a 3-element Array<int> is out of bounds →
// IndexOutOfRangeException (was a silent no-op before the fix).
BEZEL_TEST(array_bounds_1200, array_removeat_oob_high_throws_1200) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := [1, 2, 3]; "
        "  try "
        "    a.RemoveAt(10); "
        "    return a.Count; "         // reached only if no throw
        "  catch IndexOutOfRangeException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §4.5 (#1200): RemoveAt(-1) — a negative index is likewise out of bounds.
BEZEL_TEST(array_bounds_1200, array_removeat_negative_throws_1200) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := [1, 2, 3]; "
        "  try "
        "    a.RemoveAt(-1); "
        "    return a.Count; "
        "  catch IndexOutOfRangeException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §4.5 (#1200): Insert(99, v) — index > Count is out of bounds.
BEZEL_TEST(array_bounds_1200, array_insert_oob_high_throws_1200) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := [1, 2, 3]; "
        "  try "
        "    a.Insert(99, 42); "
        "    return a.Count; "
        "  catch IndexOutOfRangeException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §4.5 (#1200): Insert(-5, v) — a negative index is out of bounds.
BEZEL_TEST(array_bounds_1200, array_insert_negative_throws_1200) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := [1, 2, 3]; "
        "  try "
        "    a.Insert(-5, 42); "
        "    return a.Count; "
        "  catch IndexOutOfRangeException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §4.5 (#1200): valid RemoveAt — first, last, and middle element all work.
// RemoveAt(0) then RemoveAt(Count-1) on [10,20,30,40] → [20,30] → Count=2.
BEZEL_TEST(array_bounds_1200, array_removeat_inbounds_ok_1200) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := [10, 20, 30, 40]; "
        "  a.RemoveAt(0); "           // remove first → [20, 30, 40]
        "  a.RemoveAt(a.Count - 1); " // remove last  → [20, 30]
        "  return a.Count; "          // 2
        "end Main; end C;"), 2);
}

// §4.5 (#1200): Insert at Count (== append) is valid and must NOT throw.
// Insert(0,1) then Insert(Count,9) on [5] → [1,5,9] → Count=3.
BEZEL_TEST(array_bounds_1200, array_insert_at_count_is_valid_1200) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a: Array<int> := [5]; "
        "  a.Insert(0, 1); "          // prepend  → [1, 5]
        "  a.Insert(a.Count, 9); "    // append (index == Count) → [1, 5, 9]
        "  return a.Count; "          // 3
        "end Main; end C;"), 3);
}

// §4.5 (#1200): OwningArray<T>.RemoveAt obeys the same bounds rule (the guard
// fires before the element disposer is invoked, so no partial dispose).
BEZEL_TEST(array_bounds_1200, ownedarray_removeat_oob_throws_1200) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(new R()); a.Add(new R()); "
        "  try "
        "    a.RemoveAt(10); "        // OOB — guard fires before any dispose
        "    return a.Count; "
        "  catch IndexOutOfRangeException => return R._n; " // 0: no dispose yet
        "  end; "
        "end Main; end C;"), 0);
}
