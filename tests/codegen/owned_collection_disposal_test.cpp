// Codegen — owning-collection element disposal (issue #362, stdlib §4.5).
//
// stdlib §4.5: "`OwningArray<T>` owns each element. `Add`, `Insert`, and the
// indexer setter take ownership; `RemoveAt`, `Remove`, `Clear`, and the array's
// own `Dispose()` dispose the affected elements." A borrowing `Array<T>` stores
// borrows and never disposes its elements.
//
// (v0.213, §28.11, axiom A8 retired, #2081: the borrowing-family
// `Array<T>.DisposeElements()` en-masse element dispose — and its per-element
// manual form — were retired with axiom A8. A holder that owns its elements
// declares the owning family, whose `Dispose()`/`Clear()` cascades; selective
// cleanup takes elements out first via `Drain()`/`Pop*`/`move coll[i]`. The two
// former `DisposeElements()` runtime cases below were migrated to that owning-
// family cascade / take-out coverage.)
//
// Each test makes element disposal observable through a static counter bumped by
// the element class's destructor; the counter is returned as the exit code.
// Without the disposal the destructors never run (return 0) — so these are
// load-bearing.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
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

// stdlib §4.5: OwningArray<T>.Dispose() (here the `using var` scope-end) disposes
// every owned class element exactly once. This is the issue #362 reproducer.
BEZEL_TEST(owned_coll_dispose_4_5, owned_array_dispose_runs_each_destructor) {
    BEZEL_EXPECT_EQ(runResource(
        "do "
        "  using var a := new OwningArray<R>(); "
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
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "end; "
        "after := R._n; "
        "return after;"), 3);
}

// stdlib §4.5: OwningArray<T>.Clear() disposes every element. Encoded:
// <count right after Clear>*10 + <count after the array's own Dispose>. Clear
// disposes both (2); the subsequent scope-end Dispose finds an empty array and
// disposes nothing more (still 2) — proving no double-dispose. Expect 22.
BEZEL_TEST(owned_coll_dispose_4_5, owned_array_clear_disposes_then_no_redispose) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterClear := -1; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.Clear(); "
        "  afterClear := R._n; "
        "end; "
        "return afterClear * 10 + R._n;"), 22);
}

// stdlib §4.5: OwningArray<T>.RemoveAt(i) disposes the removed element only.
// Encoded: <count right after RemoveAt>*10 + <count after scope-end Dispose>.
// RemoveAt(1) disposes 1 of 3; scope-end disposes the other 2 → total 3.
// Expect 13 (and notably the not-removed elements are NOT double-disposed).
BEZEL_TEST(owned_coll_dispose_4_5, owned_array_remove_at_disposes_only_removed) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterRemove := -1; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.RemoveAt(1); "
        "  afterRemove := R._n; "
        "end; "
        "return afterRemove * 10 + R._n;"), 13);
}

// v0.213 (§28.11, axiom A8 retired, #2081) — migrated from the retired
// `Array<T>.DisposeElements()` case. The sound replacement for selective /
// take-out element disposal is `Drain()`: it yields each element as OWNED, and
// the loop body disposes it (`using var u := owned item;`). Both elements'
// destructors run exactly once → 2.
BEZEL_TEST(owned_coll_dispose_4_5, owning_array_drain_disposes_each) {
    BEZEL_EXPECT_EQ(runResource(
        "using var a := new OwningArray<R>(); "
        "a.Add(move (new R())); "
        "a.Add(move (new R())); "
        "for var item in a.Drain() do "
        "  using var u := owned item; "
        "end; "
        "return R._n;"), 2);
}

// stdlib §4.5 / §28.11: a borrowing Array<T>.Dispose() releases the backing
// storage but does NOT dispose the elements (they are borrows owned elsewhere).
// The elements are two `using var` R locals bound in the OUTER scope; the array
// holds a BORROW of each (§15.4/§28.11: a borrowing collection holds a borrow of
// a live local, not a fresh temp — the array is inner-scoped, so it never
// outlives the borrowed sources, satisfying §28.11's store rule). When the inner
// `do` block ends, the array is disposed while r0/r1 are still alive; a borrowing
// array skips its elements, so R._n is STILL 0 at the return — proving the array
// disposed nothing. (r0/r1 dispose later, at Main's scope end, after the return
// value is computed.) The original added fresh `new R()` temporaries to the
// borrowing array; post-#1881 those are disposed at the end of each Add statement
// (destructors run → R._n == 2, and a subsequent read would be a use-after-free),
// so the `== 0` no longer meant "disposal skipped the elements" — it meant "the
// leak masked the disposal". This rewrite restores the original property (== 0
// because the borrowing array skips its live borrowed elements) without weakening
// the assertion.
BEZEL_TEST(owned_coll_dispose_4_5, borrowing_array_dispose_skips_elements) {
    BEZEL_EXPECT_EQ(runResource(
        "using var r0 := new R(); "
        "using var r1 := new R(); "
        "do "
        "  using var a := new Array<R>(); "
        "  a.Add(r0); "
        "  a.Add(r1); "
        "end; "                 // array disposed here — borrowing → elements untouched
        "return R._n;"), 0);    // borrowed r0/r1 still alive → 0
}

// Safety: a null element must be skipped (no use-after-free / null deref).
// Migrated from the retired `Array<R?>.DisposeElements()` case to the owning-
// family cascade that still shares the same null-safe runtime helper
// (bz_array_clear_disposing): an `OwningArray<R?>` holds a null between two live
// instances; `Clear()` disposes the two live ones and skips the null → 2.
//
// Retired at declaration-site (#4796, §14.2, v0.307): `where T: class` admits
// only NON-NULLABLE reference types, so `OwningArray<R?>` is a compile error
// at the sema site. The owning-family null-skipping runtime helper
// (`bz_array_clear_disposing` for OwningArray.Clear(), `bz_array_free` for
// OwningBuffer.Dispose()) is still reachable via the §4.14 / #4679 family:
// `OwningBuffer<Foo?>` is admitted (§14.4 carve-out — storage axis, not
// element ownership), its `new OwningBuffer<Foo?>(N)` constructor leaves all
// slots null, and `Dispose()` cascades through `bz_array_free(arr, destroy)`
// whose per-element loop `if (elem_ptr) dispose_elem(elem_ptr)` skips nulls.
//
// The pre-fix `null_element_is_skipped_safely` case is preserved as a runAot
// twin at /var/tmp/bezel-scratch/r20/4796-run-row/ — the lane's codegen
// binary is on hold (#3172/#3439: codegen cold-builds dominate the budget and
// are gated to two concurrent lanes), so this is shipped to main UNRUN and
// re-runs in the integration worktree. The .bzl exercises the null-skip path
// through `OwningBuffer<Foo?>(3)` + a single `b[1] := new R()`; the runtime
// Dispose cascade disposes slot 1 and skips slots 0 and 2 → 1 destructor
// invocation. (The owning-family null-skip contract the test was guarding is
// covered for the NON-NULLABLE case by the cases above — `owned_array_*
// dispose_runs_each_destructor` and `*_clear_disposes_then_no_redispose` —
// at the same slot-count math; the null-skip path is what the runAot twin
// pins, since the §14.2 fix forbids the OwningArray route this twin's
// ancestor used.)
//
// The ownership-disposal contract this test was guarding is covered by the
// non-nullable cases above (e.g. `owned_array_dispose_runs_each_destructor`,
// `owned_array_clear_disposes_then_no_redispose`).

// stdlib §4.5: disposing an empty owning array is a no-op (and must not crash).
BEZEL_TEST(owned_coll_dispose_4_5, empty_owned_array_dispose_is_noop) {
    BEZEL_EXPECT_EQ(runResource(
        "do "
        "  using var a := new OwningArray<R>(); "
        "end; "
        "return R._n;"), 0);
}

// stdlib §4.5: an `owned` OwningArray<T> FIELD's elements are disposed when the
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
        "  private owned _items: OwningArray<R>; "
        "  public constructor() "
        "    _items := new OwningArray<R>(); "
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
