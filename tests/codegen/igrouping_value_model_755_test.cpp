// Codegen — the `IGrouping<TKey, T>` value model (issue #755, and the four
// defects #2265 records in the #1296 lowering it supersedes).
//
// Spec: stdlib §5 (`GroupBy<TKey>(key): IEnumerable<IGrouping<TKey, T>>` at
// line 3067; `IGrouping<TKey, T>: IEnumerable<T>` at 3076; `ToArray(): owned
// Array<T>` at 3070; `Single(): T; // throws if not exactly one` at 3020 and
// the `InvalidOperationException` / `IndexOutOfRangeException` fault list at
// 3089; the GroupBy paragraph at 3099, whose own worked example is
// `for var g in … do using var items := g.ToArray(); … end;`), language spec
// §29.2 (desugaring of `for`), §29.11 (enumeration), §28.9 (runtime-managed
// value disposal), v0.226.
//
// THE BUG (#755). `GroupBy` materializes each group into a `bz_grouping_t*`
// — `{ void* elements; int64 key_esz; void* key_data; }` — whose `elements`
// field is a fully-formed `bz_array_t*` the grouping owns. Nothing in the
// enumerable machinery recognised that shape, so an `IGrouping`-typed value
// was read as whatever the consuming path expected:
//
//   [17] `for var el in g do … end` (the inner loop of the canonical GroupBy
//        shape) — no for-handler admitted an IGrouping source, so it was
//        rejected: "not yet implemented: codegen for this for statement".
//   [20] `xs.GroupBy(k).SelectMany(lambda(g) => g)` — the flatten trampoline
//        wrapped the *grouping* as a lazy ARRAY source, so the array header's
//        count field landed on `key_esz`: every group yielded key_esz (= 4 for
//        an int key) elements regardless of its size. Silent wrong value; the
//        materialized groups also leaked.
//   The selector/predicate terminals — `g.Sum(sel)`, `g.Min()`, `g.Max()`,
//        `g.Any()`, `g.All(p)`, `g.Aggregate(seed, f)`, `g.Count(p)`,
//        `g.Average()` — fell through to a generic dispatch that treated the
//        grouping as an EMPTY sequence: 0 / false, silently.
//   `g.ToArray()` was untyped by sema (the §5 typing block keys on the lazy
//        sentinel's shape, and an IGrouping's element type is genericArgs[1],
//        not [0]), so `var a := g.ToArray(); a.Count` lowered to 0 AND the
//        materialized array was never reclaimed — a wrong value plus a leak,
//        on the exact idiom stdlib §5:3099 prescribes.
//   #2265: the #1296 lowering that patched a subset of the terminals by hand
//        returned the grouping's OWN array from `ToArray()` (aliasing what
//        `bz_grouping_free` frees), lowered `Single()` as a bare `arr[0]` with
//        no arity check, and `ElementAt(i)` as unchecked pointer arithmetic —
//        neither of the two faults stdlib §5:3089 requires — and admitted
//        Min/Max into its name filter without a body for them.
//
// THE FIX. An `IGrouping<TKey, T>` receiver/source is restated as its element
// `Array<T>` — TYPE and VALUE — at the three points where a sequence is
// consumed: `lowerToLazyEnumerator` (wraps `bz_grouping_elements(g)` as a
// BORROWING lazy array source, so the grouping keeps sole ownership),
// `tryLowerCollectionInstanceCall` (the eager terminals), and sema's §5 typing
// block (so a materialized result carries a real `Array<T>` type). The
// hand-rolled #1296 slot is deleted: the ORDINARY array lowerings serve the
// grouping, faults included.
//
// OBSERVABILITY. Every case runs through runAotChecked (AOT + ASan/LSan): a
// double free of the grouping's element array, a leak of a materialized copy,
// or an out-of-bounds read flips `r.ran`; the exit code pins the value, so an
// empty-sequence default (0) or a mis-sized enumeration is caught too. The
// fault cases catch the exception in-language so the disposal on the EXCEPTION
// path is exercised as well as the normal one, and each program keeps running
// afterwards (the #1864 lesson: a trailing statement after the consumption).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 10 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 10 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is still recognised as an environment condition, but it now MARKS THE
// ROW FAILED instead of hiding it. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

// ---------------------------------------------------------------------------
// [17] Inner `for var el in g` over an IGrouping — stdlib §5:3076 + spec §29.2.
// xs = [1,2,3,4] keyed `x mod 2` → groups {1,3} and {2,4}; the inner loop sums
// every element of every group → 10. Before the fix this did not compile at
// all ("not yet implemented: codegen for this for statement").
// ---------------------------------------------------------------------------
const char* kInnerForOverGrouping =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(2); xs.Add(3); xs.Add(4); "
    "    var total := 0; "
    "    for var g in xs.GroupBy(lambda(x: int) => x mod 2) do "
    "      for var el in g do total := total + el; end; "
    "    end; "
    "    Console.WriteLine(\"total=\" + total.ToString()); "
    "    return total; "
    "  end Main; "
    "end C;";

// The inner loop reads `g.Key` AFTER it — pinning that iterating a grouping
// does not consume/clobber the grouping binding itself (the lazy for-in path
// nulls a consumed lazy's slot; for an IGrouping source that slot holds the
// OUTER loop's grouping pointer and must be left alone).
// groups {1,3} (key 1) and {2,4} (key 0): 4 + 1*100 + 6 + 0*100 = 110.
const char* kInnerForThenKey =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(2); xs.Add(3); xs.Add(4); "
    "    var total := 0; "
    "    for var g in xs.GroupBy(lambda(x: int) => x mod 2) do "
    "      for var el in g do total := total + el; end; "
    "      total := total + g.Key * 100; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// [20] `GroupBy(k).SelectMany(lambda(g) => g)` — stdlib §5 Projection.
// 4 source elements in 2 groups of 2 → the flattened sequence has 4 elements
// and sums to 10 (encoded as n*10 + s = 50; the process exit code is a byte).
// Before the fix the count was 8 (key_esz = 4 per group) and the materialized
// groups leaked.
// ---------------------------------------------------------------------------
const char* kSelectManyOverGroups =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(2); xs.Add(3); xs.Add(4); "
    "    var n := xs.GroupBy(lambda(x: int) => x mod 2)"
    ".SelectMany(lambda(g) => g).Count(); "
    "    var s := xs.GroupBy(lambda(x: int) => x mod 2)"
    ".SelectMany(lambda(g) => g).Sum(); "
    "    Console.WriteLine(\"n=\" + n.ToString()); "
    "    return n * 10 + s; "   // kept under 256 — the exit code is a byte
    "  end Main; "
    "end C;";

// Uneven groups, so a per-group count is the only way to get the right answer:
// xs = [1,2,3,4,5,7] keyed `x mod 2` → odd {1,3,5,7} (4), even {2,4} (2) → 6.
const char* kSelectManyUnevenGroups =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(2); xs.Add(3); xs.Add(4); xs.Add(5); xs.Add(7); "
    "    var n := xs.GroupBy(lambda(x: int) => x mod 2)"
    ".SelectMany(lambda(g) => g).Count(); "
    "    return n; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// The selector / predicate terminals on a grouping receiver — stdlib §5.
// One group {10, 20, 30}:
//   Sum(sel)               = 60
//   Min() / Max()          = 10 / 30
//   Any()                  = true
//   All(x => x >= 10)      = true
//   Aggregate(0, +)        = 60
//   Count(x => x > 10)     = 2
// Every one of these silently returned the empty-sequence default before.
// ---------------------------------------------------------------------------
const char* kGroupingSelectorTerminals =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(10); xs.Add(20); xs.Add(30); "
    "    var r := 0; "
    "    for var g in xs.GroupBy(lambda(x: int) => 0) do "
    "      r := r + g.Sum(lambda(y: int) => y); "
    "      r := r + g.Min() + g.Max(); "
    "      if g.Any() then r := r + 1; end; "
    "      if g.All(lambda(y: int) => y >= 10) then r := r + 2; end; "
    "      r := r + g.Aggregate(0, lambda(a: int, y: int) => a + y); "
    "      r := r + g.Count(lambda(y: int) => y > 10); "
    "    end; "
    "    Console.WriteLine(\"r=\" + r.ToString()); "
    "    return r; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// #2265 item 1 — `g.ToArray()` is a FRESH copy (stdlib §5:3070 `owned
// Array<T>`), reclaimed at its own scope end and independent of the grouping.
// This is the exact idiom stdlib §5:3099 prescribes for aggregating a group
// more than once. Mutating the copy must not disturb the grouping: the copy is
// bumped by 100 per element, then the GROUP is summed again — 2 elements,
// copy sum 9 + 200 = 209, group sum still 9 → 209 + 9 = 218.
// ---------------------------------------------------------------------------
const char* kGroupingToArrayIsAFreshCopy =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(4); xs.Add(5); "
    "    var r := 0; "
    "    for var g in xs.GroupBy(lambda(v: int) => 0) do "
    "      using var items := g.ToArray(); "
    "      for var i in 0..<items.Count do items[i] := items[i] + 100; end; "
    "      r := r + items.Sum(); "
    "      r := r + g.Sum(); "
    "    end; "
    "    Console.WriteLine(\"r=\" + r.ToString()); "
    "    return r; "
    "  end Main; "
    "end C;";

// The .Count of the copy is the group's size — before the fix sema left the
// local untyped and `.Count` lowered to 0 (a silent wrong value) while the
// copy leaked. Two groups of 2 and 1 → 2 + 1 = 3.
const char* kGroupingToArrayCount =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(2); xs.Add(4); xs.Add(3); "
    "    var n := 0; "
    "    for var g in xs.GroupBy(lambda(x: int) => x mod 2) do "
    "      using var items := g.ToArray(); "
    "      n := n + items.Count; "
    "    end; "
    "    return n; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// #2265 item 2 — `Single()` faults unless the group holds exactly one element
// (stdlib §5:3020 + the §5:3089 fault list). The value form first: xs = [7]
// keyed constant → one group of one → 7.
// ---------------------------------------------------------------------------
const char* kGroupingSingleOnOneElement =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(7); "
    "    var r := 0; "
    "    for var g in xs.GroupBy(lambda(v: int) => 0) do r := g.Single(); end; "
    "    return r; "
    "  end Main; "
    "end C;";

// … and the fault form: a two-element group raises InvalidOperationException.
// Caught in-language so the exception path's disposal is exercised too (a
// leaked grouping or element array on that path flips r.ran). Returns 55.
const char* kGroupingSingleFaultsOnTwo =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(4); xs.Add(5); "
    "    var r := 0; "
    "    for var g in xs.GroupBy(lambda(v: int) => 0) do "
    "      try r := g.Single(); "
    "      catch InvalidOperationException => r := 55; "
    "      end; "
    "    end; "
    "    Console.WriteLine(\"r=\" + r.ToString()); "
    "    return r; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// #2265 item 3 — `ElementAt(i)` bounds-checks against the group's size
// (stdlib §5:3089 `IndexOutOfRangeException`). Before the fix this was
// unchecked pointer arithmetic: an ASan heap-buffer-overflow read. In-range
// first (5), then out of range → caught → 66; total 71.
// ---------------------------------------------------------------------------
const char* kGroupingElementAtBounds =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(4); xs.Add(5); "
    "    var r := 0; "
    "    for var g in xs.GroupBy(lambda(v: int) => 0) do "
    "      r := r + g.ElementAt(1); "
    "      try r := r + g.ElementAt(5); "
    "      catch IndexOutOfRangeException => r := r + 66; "
    "      end; "
    "    end; "
    "    Console.WriteLine(\"r=\" + r.ToString()); "
    "    return r; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// A grouping obtained from an element-returning terminal (`…GroupBy(k).First()`)
// is iterable too — the same value model, reached through a different producer.
// xs = [1,2,3] keyed `x mod 2` → first group {1,3} → 4.
// (runAot, not runAotChecked: the grouping the #1930 keep-path returns is a
// KNOWN leak on that path — it is deliberately not freed rather than risk a
// use-after-free, and closing it is #1930's business, not this issue's. The
// value is what this case pins.)
// ---------------------------------------------------------------------------
const char* kForOverGroupingFromFirst =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(2); xs.Add(3); "
    "    var total := 0; "
    "    var g := xs.GroupBy(lambda(x: int) => x mod 2).First(); "
    "    for var x in g do total := total + x; end; "
    "    return total; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #755 — the IGrouping<TKey, T> value model. Each case FAILS on the
// pre-fix compiler: [17] did not compile at all, [20] returned 8 (and
// leaked), the selector terminals returned the empty-sequence default,
// `ToArray()` returned an untyped/aliased array, and Single/ElementAt
// skipped their stdlib §5:3089 faults.
// =====================================================================

BEZEL_TEST(igrouping_value_model_755,
           inner_for_over_igrouping_sums_all_groups_spec_5_3076) {
    // Pre-fix: compile error "not yet implemented: codegen for this for
    // statement" at the inner `for var el in g` (finding [17]).
    JitResult r = runAotChecked(kInnerForOverGrouping);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);  // 1+3 + 2+4
}

BEZEL_TEST(igrouping_value_model_755,
           inner_for_leaves_grouping_binding_usable_spec_5_3076) {
    // Pre-fix: did not compile. Post-fix this also pins that the inner loop
    // does not null the outer loop variable's slot (`g.Key` after it).
    JitResult r = runAotChecked(kInnerForThenKey);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 110);  // (4 + 100) + (6 + 0)
}

BEZEL_TEST(igrouping_value_model_755,
           selectmany_over_groups_flattens_exactly_spec_5) {
    // Pre-fix: Count() = 8 (the int key's byte size per group, finding [20]),
    // and the materialized groups leaked (LSan).
    JitResult r = runAotChecked(kSelectManyOverGroups);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 50);  // n=4 → 40, s=1+2+3+4=10
}

BEZEL_TEST(igrouping_value_model_755,
           selectmany_over_uneven_groups_counts_per_group_spec_5) {
    // Pre-fix: 8 (4 per group × 2 groups) instead of 6.
    JitResult r = runAotChecked(kSelectManyUnevenGroups);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

BEZEL_TEST(igrouping_value_model_755,
           selector_and_predicate_terminals_on_grouping_spec_5) {
    // Pre-fix: Sum(sel), Min, Max, Any, All, Aggregate and Count(pred) all saw
    // an EMPTY sequence, so r stayed 0 (a silent wrong value, no diagnostic).
    JitResult r = runAotChecked(kGroupingSelectorTerminals);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 60 + (10+30) + 1 + 2 + 60 + 2
    BEZEL_EXPECT_EQ(r.exitCode, 165);
}

BEZEL_TEST(igrouping_value_model_755,
           grouping_toarray_is_a_fresh_owned_copy_spec_5_3070) {
    // Pre-fix: sema left `items` untyped — `items.Count` was 0, the loop body
    // never ran, and the materialized array leaked. (#2265 item 1's aliasing
    // is closed by construction: the copy is produced by the ordinary
    // materialize path, not by returning the grouping's own array.)
    JitResult r = runAotChecked(kGroupingToArrayIsAFreshCopy);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 218);  // copy 9+200, group still 9
}

BEZEL_TEST(igrouping_value_model_755,
           grouping_toarray_count_is_group_size_spec_5_3070) {
    // Pre-fix: 0 (untyped local → `.Count` lowered to 0) plus a leak.
    JitResult r = runAotChecked(kGroupingToArrayCount);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // even {2,4} = 2, odd {3} = 1
}

BEZEL_TEST(igrouping_value_model_755,
           grouping_single_returns_the_one_element_spec_5_3020) {
    JitResult r = runAotChecked(kGroupingSingleOnOneElement);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(igrouping_value_model_755,
           grouping_single_faults_on_multi_element_group_spec_5_3089) {
    // Pre-fix: `Single()` lowered as a bare arr[0] with NO arity check, so this
    // returned 4 and the catch arm was dead (#2265 item 2 — the behaviour the
    // shipped #1296 test pinned as correct).
    JitResult r = runAotChecked(kGroupingSingleFaultsOnTwo);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

BEZEL_TEST(igrouping_value_model_755,
           grouping_elementat_bounds_check_spec_5_3089) {
    // Pre-fix: `ElementAt(5)` on a 2-element group was unchecked pointer
    // arithmetic — an ASan heap-buffer-overflow READ (#2265 item 3), so this
    // test flipped r.ran rather than reaching the catch arm.
    JitResult r = runAotChecked(kGroupingElementAtBounds);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);  // 5 in range + 66 from the caught fault
}

BEZEL_TEST(igrouping_value_model_755,
           for_over_grouping_from_first_terminal_spec_5_3076) {
    // Pre-fix: compile error "not yet implemented: codegen for this for
    // statement" (the same finding [17], reached through the First() producer).
    // runAot (not Checked): the grouping returned by the #1930 keep-path is a
    // known, deliberate leak of that path — out of scope here.
    JitResult r = bezel::test::runAot(kForOverGroupingFromFirst);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // odd group {1, 3}
}
