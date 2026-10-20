// Codegen / leak — a bare `GroupBy(selector)` consumed by an element-RETURNING
// terminal (First / Single / Last / ElementAt / *OrNull — whose result IS one of
// the materialized `IGrouping` pointers) leaked the materialized groups: the outer
// `Array<IGrouping>` PLUS every grouping's element array, key buffer, and group
// object (LSan: 240 bytes / 10 allocations per GroupBy site). Issue #1930, the
// follow-up #1924 deferred. Spec: stdlib §5 (LINQ GroupBy → IEnumerable<
// IGrouping<TKey,T>>), language spec §28.9 (runtime-managed value disposal).
//
// THE BUG (#1930). #1924 fixed the SCALAR terminals (Count/Any/Sum/seeded
// Aggregate) by materializing the groups concretely and freeing them AFTER the
// fold. It EXCLUDED the element-returning terminals: their result ALIASES one of
// the grouping pointers, so freeing every grouping after the terminal would dangle
// the returned `IGrouping` (use-after-free). Those kept the borrowing snapshot path
// and still leaked the whole groups structure (the safe direction).
//
// THE FIX (#1930). Materialize the groups concretely, then free every grouping
// EXCEPT the returned one (`bz_groupby_free_groups_except`) + the outer array spine
// — the returned grouping stays live (the caller now holds it). Its OWN disposal is
// tied to the result: when it is bound to a plain `var g := …First()` that the
// escape walk proves never escapes (groupingFreeEligible_), a scope-end
// bz_grouping_free reclaims it (the `grouping` Disposal) — so the leak is fully
// gone. An escaping local (captured by a closure, added to an owning
// Array<IGrouping>, aliased) is EXCLUDED from that scope-end free — the returned
// grouping then just leaks (never a UAF, never a double-free with the owner that
// took it), and its SIBLINGS are still reclaimed by the terminal.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan; a leaked grouping /
// array flips r.ran to false, and a premature free (UAF) / double-free would trap
// (also flipping r.ran). The exit code pins the returned grouping's Key and Count,
// so a corrupted element array (e.g. a sibling free clobbering the kept group) is
// caught too. A trailing statement after the consumption follows the #1864 lesson
// (surface an otherwise-masked leak).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// `.First()` bound to a plain `var g` (the issue's primary repro), then read
// (Key + Count) and returned. xs = [1,3,2] → groups in creation order: key 1
// {1,3} (Count 2), key 0 {2} (Count 1). First() = key 1 → g.Key*100 + g.Count()
// = 1*100 + 2 = 102. The returned grouping must stay usable (its element array
// intact after the SIBLING key-0 group is freed) AND be reclaimed at scope end.
const char* kFirstBound =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(3); xs.Add(2); "
    "    var g := xs.GroupBy(lambda(x: int) => x mod 2).First(); "
    "    Console.WriteLine(\"k=\" + g.Key.ToString()); "  // trailing use — surfaces the leak
    "    return g.Key * 100 + g.Count(); "                // 1*100 + 2
    "  end Main; "
    "end C;";

// `.Last()` bound. Same groups; Last() = key 0 {2} → g.Key*100 + g.Count()
// = 0*100 + 1 = 1. Pins that freeing the OTHER (key-1, larger) group leaves the
// returned key-0 group's Key and Count correct.
const char* kLastBound =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(3); xs.Add(2); "
    "    var g := xs.GroupBy(lambda(x: int) => x mod 2).Last(); "
    "    Console.WriteLine(\"c=\" + g.Count().ToString()); "
    "    return g.Key * 100 + g.Count(); "                // 0*100 + 1
    "  end Main; "
    "end C;";

// `.ElementAt(0)` bound → the first-created group (key 1 {1,3}) →
// g.Key*100 + g.Count() = 1*100 + 2 = 102.
const char* kElementAtBound =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(3); xs.Add(2); "
    "    var g := xs.GroupBy(lambda(x: int) => x mod 2).ElementAt(0); "
    "    Console.WriteLine(\"c=\" + g.Count().ToString()); "
    "    return g.Key * 100 + g.Count(); "                // 1*100 + 2
    "  end Main; "
    "end C;";

// `.Single()` bound, over EXACTLY ONE group (xs all even → one group key 0
// {2,4,6}, Count 3). Single() returns it → g.Count() = 3. Verifies the keep-path
// when there are no siblings to free (free-none, keep the sole group, reclaim it
// at scope end).
const char* kSingleBound =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(2); xs.Add(4); xs.Add(6); "
    "    var g := xs.GroupBy(lambda(x: int) => x mod 2).Single(); "
    "    Console.WriteLine(\"k=\" + g.Key.ToString()); "
    "    return g.Count(); "                              // 3
    "  end Main; "
    "end C;";

// `.FirstOrNull()` bound to a nullable `var g`, then null-checked and used. xs all
// even → one group key 0 {2,4,2} (Count 3); FirstOrNull() is non-null → returns 3.
// The nullable-pointer keep-path: the loaded (possibly-null) pointer is passed to
// bz_groupby_free_groups_except, and the bound non-escaping `var g` is reclaimed at
// scope end (bz_grouping_free is null-safe — a no-op had the source been empty).
const char* kFirstOrNullBound =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(2); xs.Add(4); xs.Add(2); "
    "    var g := xs.GroupBy(lambda(x: int) => x mod 2).FirstOrNull(); "
    "    if g <> null then return g!.Count(); end; "      // 3
    "    return -1; "
    "  end Main; "
    "end C;";

// Soundness — the ownership-sink escape route must NOT double-free. `arr` is a
// materialized `Array<IGrouping>` (ToArray over one GroupBy → 2 groups) that OWNS
// its groupings; `arr.Add(<second GroupBy>.First())` transfers the returned
// grouping into `arr`. The `.First()` terminal frees its SECOND GroupBy's siblings
// (bz_groupby_free_groups_except) and returns the kept grouping — which `arr` now
// owns and disposes at scope end. Because the `.First()` result is UNBOUND (an
// argument, not a `var`), no scope-end bz_grouping_free is registered for it — so
// the added grouping is freed exactly once (by `arr`), never twice. `arr.Count`
// = 2 (ToArray) + 1 (Add) = 3. LSan-clean confirms no leak AND no double-free.
const char* kFirstIntoOwningArray =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(3); xs.Add(2); "
    "    var arr := xs.GroupBy(lambda(x: int) => x mod 2).ToArray(); "
    "    arr.Add(xs.GroupBy(lambda(x: int) => x mod 2).First()); "
    "    Console.WriteLine(\"n=\" + arr.Count.ToString()); "
    "    return arr.Count; "                              // 2 + 1
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1930 — a bare `GroupBy(lambda)` drained by an element-returning terminal, bound
// to a proven non-escaping local, is LSan-clean: the returned grouping's siblings
// + the outer array are freed by the terminal, and the returned grouping itself is
// reclaimed at scope end (no leak, no UAF of the returned grouping, no double-free).
// The exit code pins the returned grouping's Key + Count (element array intact).
// stdlib §5 / spec §28.9. runAotChecked = ASan/LSan oracle.
// =====================================================================

BEZEL_TEST(groupby_element_terminal_groups_leak_1930,
           bare_groupby_first_bound_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kFirstBound);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // compiled + LSan-clean (groups reclaimed once)
    BEZEL_EXPECT_EQ(r.exitCode, 102);  // First() = key 1, Count 2 → 1*100 + 2
}

BEZEL_TEST(groupby_element_terminal_groups_leak_1930,
           bare_groupby_last_bound_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kLastBound);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // Last() = key 0, Count 1 → 0*100 + 1
}

BEZEL_TEST(groupby_element_terminal_groups_leak_1930,
           bare_groupby_elementat_bound_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kElementAtBound);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 102);  // ElementAt(0) = key 1, Count 2
}

BEZEL_TEST(groupby_element_terminal_groups_leak_1930,
           bare_groupby_single_bound_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kSingleBound);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // one group {2,4,6}, Count 3
}

BEZEL_TEST(groupby_element_terminal_groups_leak_1930,
           bare_groupby_firstornull_bound_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kFirstOrNullBound);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // FirstOrNull() non-null → Count 3
}

BEZEL_TEST(groupby_element_terminal_groups_leak_1930,
           bare_groupby_first_into_owning_array_no_double_free_spec_5) {
    JitResult r = runAotChecked(kFirstIntoOwningArray);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // no leak AND no double-free (arr owns the added grouping)
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // ToArray 2 + Add 1
}
