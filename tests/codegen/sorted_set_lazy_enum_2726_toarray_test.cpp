// Spec-derived regression test for SortedSet codegen paths that funnel through
// `lowerToLazyEnumerator` (issue #2726).
//
// AUTHORITY. stdlib §4.18 (v0.278) declares on `SortedSet<T>`:
//
//     public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
//       public method ToArray(): owned Array<T>;
//       ...
//     end SortedSet;
//
// stdlib §4.6 declares on `LinkedList<T>`:
//
//     public class LinkedList<T> ...
//       public constructor(source: IEnumerable<T>);
//       ...
//     end LinkedList;
//
// stdlib §5 declares `ToArray(): owned Array<T>` on every `IEnumerable<T>`
// (the §5.2 receiver-position operator surface); spec §28.6 (collection
// ownership) and §15.4 (collection literal classification) govern the
// borrow/own/element handling. SortedSet<T> is `bz_dict_t*`-backed for every
// generic-dict-eligible element type — primitives, enums, records, and
// strings, per #1426 (the where T: IComparable<T> constraint excludes class
// elements, #2773).
//
// THE DEFECT. `src/codegen/LlvmBackend_Calls3.cpp:1522-1525`'s `headName()`
// match (the Set branch of `lowerToLazyEnumerator`) admits only
// `{Set, OwningSet}`. A `SortedSet<T>` receiver therefore MISSES the
// `bz_lazy_from_set` Set branch (the only call that treats the receiver as a
// `bz_dict_t*`) and falls through to the `bz_lazy_from_array` array path at
// line 1565 — which hands the SAME `bz_dict_t*` pointer (lowerExpr's receiver
// value, unchanged across the branches) to a function expecting `bz_array_t*`.
// The struct-prefix coincidence (`int64_t count` at offset 8 on both shapes)
// aliased the count slot correctly; the elements did NOT alias —
// `bz_array_ptr_at` reads `data[i]` (an int slot), but `bz_dict_set_gen`
// stores a malloc'd 8-byte copy of the key into `buckets[i].key` (a pointer
// to the key bytes), so the loop read bucket header bytes instead of element
// values. Every consumer that normalizes through `lowerToLazyEnumerator` —
// `sortedSet.ToArray()` (Calls3.cpp:1668), `new LinkedList<T>(sortedSet)`
// (Exprs2.cpp:3965), `sortedSet.ExceptWith(sortedSet)` — silently produced
// wrong values with no diagnostic, no crash, and no ASan report.
//
// THE FIX. Add `SortedSet` to the `headName()` match at Calls3.cpp:1522-1525
// — the SAME widening already applied to four sibling sites (Calls.cpp:2238,
// Calls2.cpp:6231, Exprs.cpp:4364, Setup.cpp:15101 / 16034 / 23682). The
// SortedSet runtime IS the `bz_dict_t*` (with sorted-ordering presentation
// layered on, #1426); `bz_lazy_from_set` snapshots the element keys the same
// way Set/OwningSet use, gated by the SAME `genericBackingType(recvTy, 0,
// isSet=true, ...)` discriminator lowerNew constructs with. Narrow residue of
// the broader #2726 Set-family thesis (which the per-fix-review verdict
// rejected for regressing 30+ tests when measured against the parent) —
// this commit ships ONLY the lowerToLazyEnumerator Set-branch widening, one
// site, one gate, one helper function.
//
// TEST. `new LinkedList<int>(sortedSet)` exercises `lowerToLazyEnumerator` on a
// SortedSet source, which is the exact path the issue body identifies as the
// "newly reachable via §4.6's source constructor" defect (#2693 made it
// reachable). After construction the LinkedList's node chain walks the
// bz_lazy_to_array-drained snapshot; each node's `Value` field is the integer
// copy. Sum of all node values discriminated against an upper bound — the
// pre-fix code returns a wrong-element-byte-pattern total (measured 84 on the
// no-fix parent for the [10,20,30,40] insert order), the post-fix code
// returns the correct ascending sum (100 for [10,20,30,40]). Discriminator:
// 100 mod 256 = 100 (the right answer); 84 mod 256 = 84 (the wrong answer).
// The Count discriminator confirms the count is right in both pre-fix and
// post-fix builds (bz_array_t/bz_dict_t struct-prefix coincidence), so the
// failure of the sum test pins the element misread and not a count misread.
//
// Per stdlib §4.18 (v0.278), spec §28.6 (collection ownership), spec §15.4
// (collection literal classification). Issue #2726 — broad Set-family
// misclassification thesis stays open for the larger registry rewrite; this
// is the structurally-isolated SortedSet portion of that thesis.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// =========================================================================
// §4.6 / stdlib §4.18 — `new LinkedList<int>(sortedSet)` walks every node
// in iteration order. Without the fix, lowerToLazyEnumerator hands a
// bz_dict_t* to bz_lazy_from_array; bz_lazy_to_array drains bucket-header
// bytes into the node chain; node.Value is wrong. With the fix, the Set
// branch matches SortedSet and routes through bz_lazy_from_set; the chain
// holds the correct ascending integer values.
// =========================================================================
BEZEL_TEST(sorted_set_lazy_enum_2726,
           linkedlist_from_sortedset_int_sums_correct_spec_4_6) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Generic; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new SortedSet<int>(); "
        "    s.Add(30); s.Add(10); s.Add(20); s.Add(40); "
        "    using var ll := new LinkedList<int>(s); "
        "    var sum := 0; "
        "    var n := ll.FirstNode; "
        "    while n <> null do "
        "      sum := sum + n!.Value; "
        "      n := n!.Next; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // Correct: 10 + 20 + 30 + 40 = 100. Measured wrong on the no-fix parent
    // (a 84-byte sum of bz_entry_t header bytes); the count slot's struct-
    // prefix coincidence read the dict's count = 4 correctly (so the loop
    // body runs some number of times), but the per-slot Value bytes read
    // bz_entry_t header bytes — the sum is garbage.
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// =========================================================================
// §4.18 — count discriminator: `new LinkedList<int>(sortedSet).Count` is
// 4 after 4 inserts. The bug (#2726) does NOT corrupt the count slot
// (bz_array_t/bz_dict_t struct-prefix coincidence aliases it correctly) —
// this test is the CONTROL that confirms the count is right in BOTH the
// pre-fix and post-fix builds, so the failure of the element-sum test above
// pins the element misread and not a count misread.
// =========================================================================
BEZEL_TEST(sorted_set_lazy_enum_2726,
           linkedlist_from_sortedset_count_matches_insertions_spec_4_18) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Generic; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new SortedSet<int>(); "
        "    s.Add(30); s.Add(10); s.Add(20); s.Add(40); "
        "    using var ll := new LinkedList<int>(s); "
        "    return ll.Count; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}