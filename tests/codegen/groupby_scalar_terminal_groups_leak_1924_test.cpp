// Codegen / leak — a bare `GroupBy(selector)` consumed by a SCALAR / AGGREGATE
// terminal (Count / Any / All / Aggregate-with-seed / …) leaked the materialized
// groups: the outer `Array<IGrouping>` PLUS every grouping's element array, key
// buffer, and group object (LSan: 240 bytes / 10 allocations per GroupBy site).
// Issue #1924 (sibling of the #1914 for-in GroupBy groups leak; reproduces with a
// plain LAMBDA selector, so it is pre-existing). Spec: stdlib §5 (LINQ GroupBy →
// IEnumerable<IGrouping<TKey,T>>), language spec §28.9 (runtime-managed value
// disposal), v0.197.
//
// THE BUG (#1924). The scalar-terminal path materialized the bare GroupBy via the
// BORROWING lazy wrap + `bz_lazy_to_array` snapshot: the snapshot drained and
// DISPOSED the lazy (own_arr == 0, so the fresh groups array was NOT freed), then
// the terminal read the snapshot and `freeLazyTemp` freed only the snapshot's
// borrowed shell — leaving the original `Array<IGrouping>` and every grouping to
// leak. #1914's owning wrap (`bz_lazy_from_owned_grouping_array`) could NOT be
// reused: the snapshot is drained BEFORE the terminal reads it, so owning at the
// lazy would use-after-free a grouping pointer an element-returning terminal
// returns.
//
// THE FIX (#1924). When the receiver is a bare GroupBy AND the terminal's result
// is NOT itself an IGrouping pointer (Count/Any/All/Aggregate-with-seed/… — the
// fold reads each grouping DURING and returns a scalar), materialize the groups
// CONCRETELY (they are already a bz_array_t* of grouping pointers — no snapshot),
// run the array terminal over them, then free the groupings
// (`bz_groupby_free_groups`) + outer array AFTER the result is computed — each
// freed exactly once, and never after a use (no UAF). Element-RETURNING terminals
// (result type IGrouping: First/Single/Last/ElementAt/*OrNull) keep the borrowing
// snapshot path — freeing there would dangle the returned grouping (deferred to
// #1930).
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan; a leaked grouping /
// array flips r.ran to false, and a premature free (UAF) / double-free would trap
// (also flipping r.ran). The exit code pins the correct terminal result, so a
// premature free would corrupt it as well. A trailing statement after the
// consumption follows the #1864 lesson (surface an otherwise-masked leak).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The exact #1924 repro: `GroupBy(lambda).Count()` PLUS `GroupBy(lambda).Any()`
// — two independent bare-GroupBy scalar-terminal sites (480 bytes / 20 allocs
// leaked before the fix). "1","3","2" → two groups by x mod 2 (odds 1,3 / even
// 2). Count() = 2 groups; Any() = true, so c := 2 + 10 = 12. Both sites' groups
// (outer array + groupings) must be freed exactly once.
const char* kCountAndAny =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(3); xs.Add(2); "
    "    var c := xs.GroupBy(lambda(x: int) => x mod 2).Count(); "
    "    if xs.GroupBy(lambda(x: int) => x mod 2).Any() then c := c + 10; end; "
    "    Console.WriteLine(\"c=\" + c.ToString()); "   // trailing stmt — surfaces the leak
    "    return c; "                                    // 2 + 10
    "  end Main; "
    "end C;";

// A SEEDED Aggregate over a bare GroupBy: the accumulator is `int` (the seed
// type), NOT an IGrouping — so the fold reads each grouping DURING and the groups
// are freed AFTER. xs = [1,3,2,4,6] → group key 1 (odds 1,3, count 2) and key 0
// (evens 2,4,6, count 3): total = 0 + (1 + 2) + (0 + 3) = 6. Pins both the
// leak-free drain and that each still-live grouping folded correctly (no UAF).
const char* kAggregateSeed =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(3); xs.Add(2); xs.Add(4); xs.Add(6); "
    "    var total := xs.GroupBy(lambda(x: int) => x mod 2)"
    "                   .Aggregate(0, lambda(acc, g) => acc + g.Key + g.Count()); "
    "    Console.WriteLine(\"total=\" + total.ToString()); "
    "    return total; "                                // 0 + (1+2) + (0+3) = 6
    "  end Main; "
    "end C;";

// A GroupBy over a LAZY source (Where) drained by Count(): the GroupBy's own
// inner source (the Where enumerator) is drained+disposed by bz_groupby_to_array,
// and the fresh groups temporary is freed by the scalar terminal — both leak-free.
// xs = [1,3,2,4,5], Where(x > 1) → [3,2,4,5], GroupBy(x mod 2) → key 1 (3,5) /
// key 0 (2,4): Count() = 2.
const char* kLazySourceCount =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(3); xs.Add(2); xs.Add(4); xs.Add(5); "
    "    var n := xs.Where(lambda(x: int) => x > 1).GroupBy(lambda(x: int) => x mod 2).Count(); "
    "    Console.WriteLine(\"n=\" + n.ToString()); "
    "    return n; "                                    // 2 groups
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1924 — a bare `GroupBy(lambda)` drained by a scalar/aggregate terminal is
// LSan-clean: the materialized groups (outer Array<IGrouping> + every grouping's
// element array, key, object) are freed exactly once AFTER the terminal computes
// its scalar result (no leak, no UAF, no double-free). stdlib §5 / spec §28.9.
// runAotChecked = ASan/LSan oracle; the exit code pins the correct result.
// =====================================================================

BEZEL_TEST(groupby_scalar_terminal_groups_leak_1924,
           bare_groupby_count_and_any_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kCountAndAny);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // compiled + LSan-clean (both sites' groups freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 12);  // Count()=2, Any()=true → 2 + 10
}

BEZEL_TEST(groupby_scalar_terminal_groups_leak_1924,
           bare_groupby_aggregate_seed_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kAggregateSeed);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);  // 0 + (1+2) + (0+3)
}

BEZEL_TEST(groupby_scalar_terminal_groups_leak_1924,
           groupby_over_lazy_source_count_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kLazySourceCount);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // Where(x>1) → GroupBy(x mod 2) → 2 groups
}
