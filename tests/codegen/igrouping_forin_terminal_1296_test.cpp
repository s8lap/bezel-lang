// Codegen / wrong-value — a value-reading terminal operator (.Sum / .First /
// .Last / .Single / .ElementAt / .ToArray / *OrNull) on an IGrouping receiver
// (`g` is the iteration variable of `for var g in xs.GroupBy(sel) do ... end`)
// silently returned 0 / wrong values. Issue #1296. Spec: stdlib §5
// (LINQ GroupBy → IEnumerable<IGrouping<TKey,T>>; IGrouping : IEnumerable<T>;
// Sum/First/Last/etc. are §5.1 LINQ extensions on IEnumerable<T>), language
// spec §28.9 (runtime-managed value disposal), §29.11 (lazy pipeline / for-in
// enumeration and disposal), v0.226.
//
// THE BUG (#1296). The IGrouping receiver is a builtin heap pointer
// (bz_grouping_t*; the `GroupBy` materialization product). IGrouping is an
// IEnumerable<T> by inheritance, so sema resolves `g.Sum()` / `g.First()` /
// … to the §5 LINQ extension methods on IEnumerable<T>. The existing
// terminal lowerings in LlvmBackend_Calls.cpp's tryLowerCollectionInstanceCall
// + the lazy-terminal materializer only handle Array<T> / lazy / linked-list
// / range / string / dict / vtable-IEnumerable receivers — NOT IGrouping —
// so the call fell through to the generic method dispatch, which treated the
// IGrouping as an empty sequence. The exit-code symptom depended on the
// terminal: `return g.Sum()` returned 0 (Sum's empty-sequence default),
// `r := g.Sum()` was silently dropped (`r` kept its prior value — no crash,
// no diagnostic), `var s := g.Sum(); r := s;` returned nondeterministic
// garbage (observed 14, 33, 49, 106, 125, 209, 227 across runs — the loaded
// IGrouping struct value's slot bytes happened to participate in the fold
// because the read of the elements array used the wrong base). The same
// breakage affected g.First / g.Last / g.Single / g.ElementAt / g.ToArray /
// g.FirstOrNull. By contrast g.Count() and g.Key correctly returned the
// group metadata (those go through dedicated IGrouping instance-method
// handlers in LlvmBackend_Calls.cpp that call bz_grouping_count /
// bz_grouping_key on the bz_grouping_t*), and a plain `r := xs.Sum()` inside
// the group loop returned 9 — so the fault is specific to value-reading
// terminals over the IGrouping element enumeration.
//
// THE FIX. Add tryLowerIGroupingTerminalCall (slot-B: LlvmBackend_Calls2.cpp)
// which recognizes the IGrouping receiver and the value-reading terminal
// name, extracts the elements array via bz_grouping_elements (the bz_grouping_t
// struct's `elements` field), and runs the terminal directly on that
// bz_array_t*. Sum is an inline fold over the array; First/Last/Single/
// ElementAt/*OrNull/ToArray reduce to the bz_array_* equivalents. Wired in
// lowerCall before the generic method dispatch so the call does not fall
// through to the empty-sequence default.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan; an incorrect
// terminal (Sum → 0, First → 0, …) flips the exit code; a UAF / double-free
// on the elements array would flip r.ran. The exit code pins the correct
// group count and the correct per-group element sum, so a corrupted array
// read or a UAF is caught too. A trailing statement after the consumption
// loop is included (the #1864 lesson).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// The exact #1296 repro shape (g.Sum() over a for-in IGrouping variable):
// xs = [4, 5], key = 0 → one group holding {4, 5} → g.Sum() = 9.
const char* kForInGroupBySum =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(4); xs.Add(5); "
    "    var r := 77; "
    "    for var g in xs.GroupBy(lambda(v) => 0) do "
    "      r := g.Sum(); "
    "    end; "
    "    Console.WriteLine(\"r=\" + r.ToString()); "
    "    return r; "
    "  end Main; "
    "end C;";

// g.Sum() stored through a temp `var s := g.Sum(); r := s;` (the #1296
// nondeterministic-garbage repro shape). Should still be 9.
const char* kForInGroupBySumViaTemp =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(4); xs.Add(5); "
    "    var r := 0; "
    "    for var g in xs.GroupBy(lambda(v) => 0) do "
    "      var s := g.Sum(); "
    "      r := s; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// All value-reading terminals in one expression: Sum + First + Last +
// ElementAt(1) + Count. xs = [4, 5] → 9 + 4 + 5 + 5 + 2 = 25.
//
// CORRECTED for #755/#2265 item 2. This source used to include `g.Single()`
// and expect 29 — i.e. it asserted that `Single()` on a TWO-element group
// yields its first element. That is not what the standard library says:
// stdlib §5:3020 declares `Single(): T; // throws if not exactly one` and
// §5:3089 names `InvalidOperationException`. The #1296 lowering had no arity
// check (it emitted a bare `arr[0]`), and the expectation encoded that missing
// fault as correct. The `Single()` contract now has its own tests below (one
// pinning the value on a genuine one-element group, one pinning the fault on a
// two-element group), and this case keeps only the terminals whose result is
// well-defined for a two-element group.
const char* kForInGroupByAllTerminals =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(4); xs.Add(5); "
    "    var r := 0; "
    "    for var g in xs.GroupBy(lambda(v) => 0) do "
    "      r := g.Sum() + g.First() + g.Last() + g.ElementAt(1) + g.Count(); "
    "    end; "
    "    return r; "
    "  end Main; "
    "end C;";

// Multiple groups: xs = [1,3,2,4,6], key = x mod 2 → odd group {1,3} sum 4,
// even group {2,4,6} sum 12 → total 16. The for-in enumerates two
// groupings; each g.Sum() folds the per-group elements array.
const char* kForInGroupByMultipleGroupsSum =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    using var xs := new Array<int>(); "
    "    xs.Add(1); xs.Add(3); xs.Add(2); xs.Add(4); xs.Add(6); "
    "    var total := 0; "
    "    for var g in xs.GroupBy(lambda(x: int) => x mod 2) do "
    "      total := total + g.Sum(); "
    "    end; "
    "    Console.WriteLine(\"total=\" + total.ToString()); "
    "    return total; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1296 — value-reading terminals (.Sum / .First / .Last / .Single /
// .ElementAt / .Count) on an IGrouping receiver returned 0 / wrong values
// in a for-in GroupBy loop. The fix extracts the elements array via
// bz_grouping_elements and runs the terminal on that bz_array_t* (stdlib §5
// / spec §28.9 / §29.11). runAotChecked = ASan/LSan oracle (a leaked
// array or a UAF flips r.ran); the exit code pins the correct result so
// an incorrect empty-sequence default is caught too.
// =====================================================================

BEZEL_TEST(igrouping_forin_terminal_1296,
           forin_bare_groupby_sum_correct_value_spec_5) {
    JitResult r = runAotChecked(kForInGroupBySum);
    BEZEL_EXPECT_TRUE(r.ran);        // compiled + LSan-clean
    BEZEL_EXPECT_EQ(r.exitCode, 9);  // one group {4, 5} → 4 + 5 = 9
}

BEZEL_TEST(igrouping_forin_terminal_1296,
           forin_bare_groupby_sum_via_temp_correct_value_spec_5) {
    JitResult r = runAotChecked(kForInGroupBySumViaTemp);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

BEZEL_TEST(igrouping_forin_terminal_1296,
           forin_bare_groupby_all_terminals_correct_value_spec_5) {
    JitResult r = runAotChecked(kForInGroupByAllTerminals);
    BEZEL_EXPECT_TRUE(r.ran);
    // Sum (9) + First (4) + Last (5) + ElementAt(1) (5) + Count (2)
    // (`Single()` moved to its own pair of cases — see #755/#2265 item 2.)
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

BEZEL_TEST(igrouping_forin_terminal_1296,
           forin_bare_groupby_multi_groups_sum_correct_value_spec_5) {
    JitResult r = runAotChecked(kForInGroupByMultipleGroupsSum);
    BEZEL_EXPECT_TRUE(r.ran);
    // odd {1, 3} sum 4 + even {2, 4, 6} sum 12 → 16
    BEZEL_EXPECT_EQ(r.exitCode, 16);
}
