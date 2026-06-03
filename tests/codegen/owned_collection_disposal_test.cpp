// Codegen — owning-collection element disposal (issue #362, stdlib §4.5).
//
// stdlib §4.5: "`OwnedArray<T>` owns each element. `Add`, `Insert`, and the
// indexer setter take ownership; `RemoveAt`, `Remove`, `Clear`, and the array's
// own `Dispose()` dispose the affected elements." And: "`Array<T>` stores
// borrows … The array's `Dispose()` … does not dispose the elements. To dispose
// owned class instances … call `DisposeElements()` (which disposes each
// class-typed element then clears)."
//
// Each test makes element disposal observable through a static counter bumped by
// the element class's destructor; the counter is returned as the exit code.
// Without the fix every Dispose/Clear/RemoveAt/DisposeElements case returns 0
// (the element destructors never run) — so these are load-bearing.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// A resource class R whose destructor bumps the static counter R._n, wrapped in
// a holder class C with the given Main body. The Main body returns the counter
// (or an encoded combination) so disposal is observable via the exit code.
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

// stdlib §4.5: OwnedArray<T>.Dispose() (here the `using var` scope-end) disposes
// every owned class element exactly once. This is the issue #362 reproducer.
BEZEL_TEST(owned_coll_dispose_4_5, owned_array_dispose_runs_each_destructor) {
    BEZEL_EXPECT_EQ(runResource(
        "do "
        "  using var a := new OwnedArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "end; "
        "return R._n;"), 2);
}

// stdlib §4.5: each element is disposed EXACTLY once — disposing the array does
// not re-dispose. Encoded: <count after scope>*10 + <count> ; both are 3.
BEZEL_TEST(owned_coll_dispose_4_5, owned_array_dispose_each_exactly_once) {
    BEZEL_EXPECT_EQ(runResource(
        "var after := -1; "
        "do "
        "  using var a := new OwnedArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "end; "
        "after := R._n; "
        "return after;"), 3);
}

// stdlib §4.5: OwnedArray<T>.Clear() disposes every element. Encoded:
// <count right after Clear>*10 + <count after the array's own Dispose>. Clear
// disposes both (2); the subsequent scope-end Dispose finds an empty array and
// disposes nothing more (still 2) — proving no double-dispose. Expect 22.
BEZEL_TEST(owned_coll_dispose_4_5, owned_array_clear_disposes_then_no_redispose) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterClear := -1; "
        "do "
        "  using var a := new OwnedArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.Clear(); "
        "  afterClear := R._n; "
        "end; "
        "return afterClear * 10 + R._n;"), 22);
}

// stdlib §4.5: OwnedArray<T>.RemoveAt(i) disposes the removed element only.
// Encoded: <count right after RemoveAt>*10 + <count after scope-end Dispose>.
// RemoveAt(1) disposes 1 of 3; scope-end disposes the other 2 → total 3.
// Expect 13 (and notably the not-removed elements are NOT double-disposed).
BEZEL_TEST(owned_coll_dispose_4_5, owned_array_remove_at_disposes_only_removed) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterRemove := -1; "
        "do "
        "  using var a := new OwnedArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.RemoveAt(1); "
        "  afterRemove := R._n; "
        "end; "
        "return afterRemove * 10 + R._n;"), 13);
}

// stdlib §4.5: Array<T>.DisposeElements() disposes each class-typed element
// then clears. Array<T> is borrowing, so this is the explicit en-masse dispose.
BEZEL_TEST(owned_coll_dispose_4_5, array_dispose_elements_disposes_each) {
    BEZEL_EXPECT_EQ(runResource(
        "using var a := new Array<R>(); "
        "a.Add(new R()); "
        "a.Add(new R()); "
        "a.DisposeElements(); "
        "return R._n;"), 2);
}

// stdlib §4.5: a borrowing Array<T>.Dispose() releases the backing storage but
// does NOT dispose the elements (they are borrows owned elsewhere). The
// `using var` scope-end frees the array without touching the elements → 0.
BEZEL_TEST(owned_coll_dispose_4_5, borrowing_array_dispose_skips_elements) {
    BEZEL_EXPECT_EQ(runResource(
        "do "
        "  using var a := new Array<R>(); "
        "  a.Add(new R()); "
        "  a.Add(new R()); "
        "end; "
        "return R._n;"), 0);
}

// Safety: a null element must be skipped (no use-after-free / null deref).
// A borrowing Array<R?> holds a null between two live instances;
// DisposeElements must dispose the two live ones and skip the null → 2.
BEZEL_TEST(owned_coll_dispose_4_5, null_element_is_skipped_safely) {
    BEZEL_EXPECT_EQ(runResource(
        "using var a := new Array<R?>(); "
        "a.Add(new R()); "
        "a.Add(null); "
        "a.Add(new R()); "
        "a.DisposeElements(); "
        "return R._n;"), 2);
}

// stdlib §4.5: disposing an empty owning array is a no-op (and must not crash).
BEZEL_TEST(owned_coll_dispose_4_5, empty_owned_array_dispose_is_noop) {
    BEZEL_EXPECT_EQ(runResource(
        "do "
        "  using var a := new OwnedArray<R>(); "
        "end; "
        "return R._n;"), 0);
}

// stdlib §4.5: an `owned` OwnedArray<T> FIELD's elements are disposed when the
// owner is disposed (its compiler-generated Dispose cascades through the field's
// backing into each class element). Disposing the owner (a using var) here must
// run both element destructors → 2.
BEZEL_TEST(owned_coll_dispose_4_5, owned_array_field_disposed_with_owner) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class Pool "
        "  private owned _items: OwnedArray<R>; "
        "  public constructor() "
        "    _items := new OwnedArray<R>(); "
        "    _items.Add(move (new R())); "
        "    _items.Add(move (new R())); "
        "  end constructor; "
        "end Pool; "
        "class C public static method Main(): int "
        "  do "
        "    using var p := new Pool(); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 2);
}
