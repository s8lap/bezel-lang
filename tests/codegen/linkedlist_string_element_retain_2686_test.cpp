// Codegen / disposal — a borrowing `LinkedList<string>` RELEASED a +1 that NO
// insert path ever retained. Issue #2686.
//
// Spec: §28.11 "Collection element ownership" — the element-slot policy table
// classifies `string` as a "runtime-managed reference" whose BORROWING-family
// slot is a "shared reference", and the operation table says `store` "stores the
// reference"; §28.9 "Runtime-managed types" (a `string` is refcounted — every
// co-owner takes a reference and releases exactly one); §15.4 "Collection
// literals" (a literal CONSTRUCTS the collection — the same construction
// `new` + `.AddLast` writes, so it must take the same references).
// Spec revision v0.237.
//
// THE BUG (opposite sign to the sibling #2682 dictionary defect). For a
// `LinkedList<string>`, ZERO insert paths retained — `.AddFirst` / `.AddLast` /
// `.AddBefore` / `.AddAfter` (LlvmBackend_Calls3.cpp) and the LinkedList arm of
// `materializeHeapCollectionLiteral` (LlvmBackend_Setup.cpp), which even
// documented it ("Like `.AddLast`, no string-retain"). `grep -c emitStringRetain
// LlvmBackend_Calls3.cpp` returned 0. Yet TWO disposal sites installed
// `stringElementReleaseThunk()` anyway: `buildCollectionTempDisposal`
// (LlvmBackend_Setup.cpp — the owned temp a collection literal in CALL-ARGUMENT
// position becomes) and `patternAResultCollectionDisposer`
// (LlvmBackend_Disposal.cpp — an async Pattern-A task result). So such a list
// consumed a reference it never took:
//   AddressSanitizer: SEGV on unknown address 0x000000000004
//     #0 __asan::Allocator::Deallocate  #2 bz_string_release
//     #3 bz$strElemRelease              #4 bz_llist_free
//
// THE FIX. Add the missing RETAIN at all five insert paths rather than deleting
// the two releases: §28.11's table makes a borrowing collection of strings a
// holder of shared references, so co-ownership is the specified direction, and
// deleting the releases would entrench the divergence. The guard is a verbatim
// mirror of the Array/Stack/Queue element arm (`litIsStrArr`): `OwningLinkedList`
// is excluded (its element parameter is `owned` — a transfer, and
// `elementPolicyFor` puts an owning-family string in the Value row), `string?` is
// excluded, and a fresh +1 producer TRANSFERS via `isPlusOneStringProducer`
// instead of double-retaining. The `using var` / auto-drop disposal path
// (`registerUsingDisposal`) gains the matching RELEASE, so the dominant
// construct-then-drop shape is balanced end to end.
//
// ⚠ WHY THE STRING MUST BE BUILT AT RUNTIME. `"ab" + "cd"` constant-folds to an
// RC-SENTINEL string and `bz_string_release` is a NO-OP on a sentinel, so a
// literal-built probe passes whether or not the bug is present. Every program
// below builds its string with interpolation at run time (`$"v-{i}"`).
//
// ⚠ WHY THE SHARED STRING IS A PARAMETER, NOT A LOCAL. A `var s: string := $"…"`
// that escapes into a collection literal is dropped from `stringReleaseEligible_`
// (#422) and leaks its +1 — PRE-EXISTING and unrelated to #2686 (measured on the
// issue's own repro: 731 B / 40 allocations, identical to the figure #2682
// recorded for the dictionary form). LSan would otherwise fail these tests for
// that unrelated reason. Passing the string as a borrowed PARAMETER keeps the
// caller's +1 accounted for, so these programs are exactly leak-neutral and
// `runAotChecked` observes ONLY the #2686 balance.
//
// OBSERVABILITY. `runAotChecked` links and runs the generated code under
// ASan/LSan, so BOTH directions flip `r.ran` to false: a double-free / SEGV (too
// many releases — the bug) and a leak (too many retains — an over-correction).
// The exit code independently pins the lengths read back OUT of the lists, so a
// premature free corrupts it regardless of the sanitizer.

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

// (A) The #2686 repro, reduced to a leak-neutral shape: a collection literal in
// CALL-ARGUMENT position becomes an owned temp whose disposal releases each
// string element. Pre-fix nothing retained, so the temp consumed a +1 it never
// took → ASan SEGV inside bz_llist_free. The same borrowed string is used by two
// separate literals so a stale free is hit repeatedly, and 20 iterations make it
// certain rather than tolerated once.
// Value: 20 iterations x 3 x len("v-i"); len sums to 71 over 1..20 → 213.
const char* kLiteralTempReleasesUnretainedString =
    "module M; class C "
    "  public static method Sum(items: LinkedList<string>): int "
    "    var n := 0; "
    "    for var it in items do "
    "      n := n + it.Length; "
    "    end; "
    "    return n; "
    "  end Sum; "
    "  public static method Two(s: string): int "
    "    return Sum([s, s]) + Sum([s]); "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "   // runtime-built, NOT folded
    "    end; "
    "    return total; "                            // 213
    "  end Main; "
    "end C;";

// (B) The `.AddFirst` / `.AddLast` insert paths paired with the `using var`
// disposal path. This is the OTHER half of the balance: construction now retains,
// so `registerUsingDisposal` must release or every such list leaks one reference
// per element. Two lists share one borrowed string, so an unpaired release here
// would double-free instead.
// Value: 20 iterations x (2 + 2) = 80.
const char* kAddFirstAddLastUsingVarBalanced =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    using var a: LinkedList<string> := new LinkedList<string>(); "
    "    a.AddLast(s); "
    "    a.AddFirst(s); "
    "    using var b: LinkedList<string> := new LinkedList<string>(); "
    "    b.AddLast(s); "
    "    b.AddFirst(s); "
    "    return a.Count + b.Count; "                // 4
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 80
    "  end Main; "
    "end C;";

// (C) The node-anchored inserts `.AddBefore` / `.AddAfter` — the remaining two of
// the five insert paths. Their element is argument 1 (argument 0 is the anchor
// node), which is the axis a careless mirror of `.AddLast` gets wrong: retaining
// the NODE instead of the element would corrupt the list rather than the string.
// The lengths are read back out of the list to pin that the elements survive.
// Value: 20 iterations x 3 x len = 213.
const char* kAddBeforeAddAfterRetainElementNotNode =
    "module M; class C "
    "  public static method Three(s: string): int "
    "    using var l: LinkedList<string> := new LinkedList<string>(); "
    "    var mid := l.AddLast(s); "
    "    l.AddBefore(mid, s); "
    "    l.AddAfter(mid, s); "
    "    var n := 0; "
    "    for var it in l do "
    "      n := n + it.Length; "
    "    end; "
    "    return n; "                                // 3 * len(s)
    "  end Three; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Three($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 213
    "  end Main; "
    "end C;";

// (D) The TRANSFER arm, and the over-correction guard: every element expression
// here is itself a fresh +1 producer (`$"v-{i}"` inline), so the fix must NOT
// retain — the list takes over the producer's reference. A fix that retained
// unconditionally would leak one string per element and LSan would fail this.
// Covers both the literal arm and `.AddLast`.
// Value: 20 iterations x 3 x len = 213.
const char* kProducerElementTransfersNotRetained =
    "module M; class C "
    "  public static method Sum(items: LinkedList<string>): int "
    "    var n := 0; "
    "    for var it in items do "
    "      n := n + it.Length; "
    "    end; "
    "    return n; "
    "  end Sum; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + Sum([$\"v-{i}\", $\"v-{i}\"]); "
    "      using var l: LinkedList<string> := new LinkedList<string>(); "
    "      l.AddLast($\"v-{i}\"); "
    "      total := total + Sum(l); "
    "    end; "
    "    return total; "                            // 213
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #2686 repro — a LinkedList<string> literal in call-argument position no
// longer double-frees the string its temp disposal releases.
// §28.11 element-slot table / §28.9 / §15.4.
// =====================================================================
BEZEL_TEST(codegen_linkedlist_string_element_retain_2686,
           literal_temp_no_double_free_of_shared_string_spec_28_11) {
    JitResult r = runAotChecked(kLiteralTempReleasesUnretainedString);
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free (the bug) and no leak
    BEZEL_EXPECT_EQ(r.exitCode, 213);  // every element still readable
}

// =====================================================================
// (B) `.AddFirst`/`.AddLast` construction paired with `using var` disposal is
// balanced in both directions. §28.11 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_linkedlist_string_element_retain_2686,
           add_first_last_with_using_var_disposal_balanced_spec_28_9) {
    JitResult r = runAotChecked(kAddFirstAddLastUsingVarBalanced);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 80);
}

// =====================================================================
// (C) `.AddBefore`/`.AddAfter` retain the ELEMENT (argument 1), not the anchor
// node. §28.11.
// =====================================================================
BEZEL_TEST(codegen_linkedlist_string_element_retain_2686,
           add_before_after_retain_element_not_node_spec_28_11) {
    JitResult r = runAotChecked(kAddBeforeAddAfterRetainElementNotNode);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 213);
}

// =====================================================================
// (D) A +1-producer element is TRANSFERRED, not retained — the over-correction
// guard (an unconditional retain leaks here). §15.4 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_linkedlist_string_element_retain_2686,
           producer_element_transfers_without_extra_retain_spec_28_9) {
    JitResult r = runAotChecked(kProducerElementTransfersNotRetained);
    BEZEL_EXPECT_TRUE(r.ran);         // no leak from a double-retain
    BEZEL_EXPECT_EQ(r.exitCode, 213);
}
