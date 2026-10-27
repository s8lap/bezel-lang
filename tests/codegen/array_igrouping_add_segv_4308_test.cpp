// Spec-derived regression test for #4308 —
// `Array<IGrouping<K,V>>.Add(g)` SIGSEGVs on a real grouping produced by
// `xs.GroupBy(...)`.
//
// AUTHORITY. spec §5 (v0.298) / stdlib §5 define `IGrouping<TKey, T> : IEnumerable<T>`
// and stdlib §5 declares `readonly property Key: TKey;` plus the §29
// enumerable contract. spec §15.4 governs heap collections — `Array<T>` is a
// BORROWING collection, so its slot stores an element reference WITHOUT
// disposal obligation on the array side. spec §28.11 v0.135 fixes the §28.11
// table to {value, RuntimeManaged, ClassBorrow, ClassOwned}; borrowing class /
// interface elements MUST land on the ClassBorrow row.
//
// THE DEFECT. `LlvmBackend_ElementPolicy.cpp::elementPolicyFor` had a
// pre-existing arm for `IGrouping` that *unconditionally* returned ClassOwned
// with `groupingFreeThunk()` as the disposer, regardless of whether the
// containing collection was owning or borrowing. With `Array<IGrouping>` this
// meant: at scope-end, the array's auto-disposal ran `bz_array_free(arr,
// groupingFreeThunk)` which iterated the slots and called `bz_grouping_free`
// on each. But the groupings were ALREADY freed by the source LINQ lazy's
// own `bz_groupby_free_groups` at the lazy's scope-end — the producer (LINQ)
// owns them (per stdlib §5: "All are lazy unless documented otherwise"). The
// array's second free hit memory whose `elements` field had already been
// recycled; `bz_grouping_free` dereferenced it, `bz_array_free` then did
// `free(arr->data)` on the recycled pointer — measured under ASan as
// `SEGV on address 0x1b` deep inside the inner bz_array_free's free path.
//
// The ClassOwned / ClassBorrow row of the §28.11 table is the same row every
// other class / interface element sits on; the IGrouping arm was the lone
// un-gated branch. Fix: gate the disposer on `owningFamily`, mirroring the
// Subscription sibling arm directly below it. OwningArray<IGrouping> still
// disposes (the transfer took ownership); borrowing Array<IGrouping> just
// borrows, no slot-side disposal.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::JitResult;

namespace {

// The verbatim filed repro (#4308): a `for var g in xs.GroupBy(...)` driving
// `groups.Add(g)` on a borrowing `Array<IGrouping<int, int>>`, with
// `xs = [1, 2]`. The two-element source has TWO distinct key buckets
// (`1 mod 2` vs `2 mod 2`), so `groups.Count` must be 2.
const char* kForInAdd =
    "module M; import System.Collections.Linq; "
    "class C public static method Main(): int "
    "  var xs := new Array<int>(); "
    "  xs.Add(1); xs.Add(2); "
    "  using var groups := new Array<IGrouping<int, int>>(); "
    "  for var g in xs.GroupBy(lambda(x: int) => x mod 2) do "
    "    groups.Add(g); "
    "  end; "
    "  return groups.Count; "
    "end Main; end C;";

// Same shape, but the IGrouping is reached via `.ToArray()` + indexer
// (`groupings[i]`) rather than `for var g in`. The element-policy bug is at
// the `Array<IGrouping>` slot, independent of HOW the IGrouping pointer
// reaches the Add call.
const char* kToArrayIndexerAdd =
    "module M; import System.Collections.Linq; "
    "class C public static method Main(): int "
    "  var xs := new Array<int>(); "
    "  xs.Add(1); xs.Add(2); "
    "  var groupings := xs.GroupBy(lambda(x: int) => x mod 2).ToArray(); "
    "  using var groups := new Array<IGrouping<int, int>>(); "
    "  var i := 0; "
    "  while i < groupings.Count do "
    "    groups.Add(groupings[i]); "
    "    i := i + 1; "
    "  end; "
    "  return groups.Count; "
    "end Main; end C;";

// CONTROL: a borrowing Array<Widget>.Add(w) for a plain class element was
// already correct on HEAD (#4308 is interface-typed and goes through a
// different element-policy arm); this stays GREEN through the fix to prove
// the gated IGrouping branch did not regress the plain-class path.
const char* kPlainClassControl =
    "module M; import System.Collections.Generic; "
    "class Widget public v: int; "
    "  public constructor() end constructor; "
    "  public method Tag(): int => 1; "
    "end Widget; "
    "class C public static method Main(): int "
    "  using var arr := new Array<Widget>(); "
    "  arr.Add(new Widget()); "
    "  return arr.Count; "
    "end Main; end C;";

}  // namespace

// ── §15.4 / §5 / §28.11 v0.135 — the filed repro (plain) ────────────────────

// Pre-fix: SIGSEGV (exit 139) on scope-end dispose; `groups.Count` returns the
// right value (2) only if Add succeeded, which it did — the crash is in the
// OWNED-LIKE disposal of a BORROWED collection, see header comment.
BEZEL_TEST(array_igrouping_add_segv_4308, for_in_add_count_clean_exit) {
    JitResult r = runAot(kForInAdd);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// ── §15.4 / §5 / §28.11 v0.135 — the filed repro (ASan memory net) ──────────

// Same probe under `runAotChecked`: the SIGSEGV that the plain run reports
// becomes an explicit ASan SEGV at bz_array_free:298 (free of an
// already-recycled chunk's payload), pinning the double-free against the
// lazy's bz_groupby_free_groups. Post-fix both runs exit 2 cleanly.
BEZEL_TEST(array_igrouping_add_segv_4308, for_in_add_count_clean_exit_asan) {
    JitResult r = runAotChecked(kForInAdd);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// ── §15.4 / §5 / §28.11 v0.135 — `ToArray()` + indexer ──────────────────────

// Companion control that varies the IGrouping-reach path. Pre-fix this also
// crashed at scope-end (same slot-side element policy), confirming the bug is
// at the array-side policy, not at how `g` was obtained.
BEZEL_TEST(array_igrouping_add_segv_4308, toarray_indexer_add_count) {
    JitResult r = runAot(kToArrayIndexerAdd);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// ── CONTROL: a plain class element is unaffected by the gated IGrouping
// branch (it lives on the class/interface row of the §28.11 table, just like
// every other class) ─────────────────────────────────────────────────────────

BEZEL_TEST(array_igrouping_add_segv_4308, control_plain_class_add_still_works) {
    JitResult r = runAot(kPlainClassControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
