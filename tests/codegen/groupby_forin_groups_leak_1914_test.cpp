// Codegen / leak — a bare `for var g in seq.GroupBy(selector)` leaked the fresh
// GroupBy result: the selector closure cell (already released) PLUS the
// materialized groups — the outer `Array<IGrouping>` and every grouping's
// element array, key buffer, and group object. Issue #1914 (a) (discovered while
// fixing #1758; reproduces with a plain LAMBDA selector, so it is pre-existing
// and independent of #1758's primitive-intrinsic method references). Spec:
// stdlib §5 (LINQ GroupBy → IEnumerable<IGrouping<TKey,T>>), language spec §28.9
// (runtime-managed value disposal), §29.11 (lazy pipeline / for-in enumeration
// and disposal), v0.196.
//
// THE BUG (#1914 a). `lowerToLazyEnumerator`'s bare-GroupBy branch materialized
// the groups via `bz_groupby_to_array` (a fresh `Array<IGrouping>` no name owns)
// and wrapped it with the BORROWING `bz_lazy_from_array` (own_arr == 0). The
// for-in drains and disposes the enumerator at loop end, but `bz_lazy_dispose`
// never freed the borrowed array — so the outer array AND every grouping leaked
// (LSan: 40-byte direct + several indirect — group array / node / key
// allocations). The `.ToArray()` form re-owns the groupings through its produced
// `Array<IGrouping>` local, so only the for-in / scalar-terminal drains leaked.
//
// THE FIX. The for-in (a DRAIN-then-DISPOSE consumer that reads each grouping
// only DURING iteration and disposes only at loop end) OWNS the groups temporary:
// `lowerToLazyEnumerator(..., ownFreshGroupings=true)` wraps it with the new
// `bz_lazy_from_owned_grouping_array`, and `bz_lazy_dispose` frees every grouping
// (`bz_groupby_free_groups`) then the outer array — exactly once (dispose runs
// once per enumerator). ToArray and the scalar-terminal snapshot paths keep the
// borrowing wrap (owning there would double-free the re-owned groupings, or read
// a grouping pointer after the materialize-and-dispose snapshot, respectively).
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan; a leaked grouping /
// array or a double-free/UAF flips r.ran to false. A trailing statement after the
// consumption loop is included (the #1864 lesson: it reliably surfaces an
// otherwise-register-masked leak) — though the groups-array leak here is
// reachable regardless. The exit code pins the correct enumeration result, so a
// premature free (UAF) would corrupt it as well as flip r.ran.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The exact #1914 (a) repro shape: a bare `for var g in strings.GroupBy((s) =>
// int.Parse(s))` — the selector is a plain LAMBDA — with a body that USES each
// grouping (Key + Count, so a premature free would UAF), plus a TRAILING
// statement. "1","1","2" → two groups: key 1 (count 2), key 2 (count 1). The loop
// counts the groups (n = 2); each grouping is live throughout its iteration and
// the whole groups temporary is freed exactly once at loop end.
const char* kForInGroupByLambdaSelector =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var strings := new Array<string>(); "
    "    strings.Add(\"1\"); strings.Add(\"1\"); strings.Add(\"2\"); "
    "    var n := 0; "
    "    var keys := 0; "
    "    for var g in strings.GroupBy(lambda(s) => int.Parse(s)) do "
    "      n := n + 1; "
    "      keys := keys + g.Key * g.Count(); "   // reads a live grouping — UAF if freed early
    "    end; "
    "    Console.WriteLine(\"n=\" + n.ToString()); "   // trailing stmt — surfaces the leak
    "    return n; "                                    // two distinct keys
    "  end Main; "
    "end C;";

// A value-typed variant: `for var g in xs.GroupBy((x) => x mod 2)` accumulating
// Key + Count() per group. xs = [1,3,2,4,6] → group key 1 (odds 1,3, count 2) and
// key 0 (evens 2,4,6, count 3): total = (1 + 2) + (0 + 3) = 6. Pins both the
// leak-free drain and the correct enumeration of each still-live grouping.
const char* kForInGroupByValueUse =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(3); xs.Add(2); xs.Add(4); xs.Add(6); "
    "    var total := 0; "
    "    for var g in xs.GroupBy(lambda(x: int) => x mod 2) do "
    "      total := total + g.Key + g.Count(); "
    "    end; "
    "    Console.WriteLine(\"total=\" + total.ToString()); "
    "    return total; "                                 // (1+2) + (0+3) = 6
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1914 (a) — a bare `for var g in seq.GroupBy(lambda)` is LSan-clean: the
// materialized groups (outer Array<IGrouping> + every grouping's element array,
// key, and object) are freed exactly once at loop end, and each grouping is live
// throughout its own iteration (no UAF). stdlib §5 / language spec §28.9 / §29.11.
// runAotChecked = ASan/LSan oracle (a leaked grouping or a double-free flips
// r.ran); the exit code pins the correct group count.
// =====================================================================

BEZEL_TEST(groupby_forin_groups_leak_1914,
           forin_bare_groupby_lambda_selector_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kForInGroupByLambdaSelector);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // compiled + LSan-clean (groups freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // "1","1","2" → two distinct keys
}

BEZEL_TEST(groupby_forin_groups_leak_1914,
           forin_bare_groupby_key_and_count_use_lsan_clean_spec_5) {
    JitResult r = runAotChecked(kForInGroupByValueUse);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);  // (1+2) + (0+3)
}
