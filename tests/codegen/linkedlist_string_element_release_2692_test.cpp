// Codegen / disposal — the RELEASE half of `LinkedList<string>` element
// co-ownership. Issue #2692 (follow-up to #2686).
//
// Spec: §28.11 "Collection element ownership" — the element-slot policy table
// classifies `string` as a "runtime-managed reference" whose BORROWING-family
// slot is a "shared reference"; the operation table says `store` "stores the
// reference" and `remove`/`free` drop it. §28.9 "Runtime-managed types" (a
// `string` is refcounted — every co-owner takes exactly one reference and
// releases exactly one). §15.4 "Collection literals" (a literal CONSTRUCTS the
// collection, so it takes the same references the `new` + `.AddLast` writes do).
// Spec revision v0.240.
//
// THE HISTORY. #2686 fixed the reported DOUBLE-FREE by completing the RETAIN
// side in full — all five insert paths (`.AddFirst`, `.AddLast`, `.AddBefore`,
// `.AddAfter` in LlvmBackend_Calls3.cpp, and the `bz_llist_add_last` arm of
// `materializeHeapCollectionLiteral` in LlvmBackend_Setup.cpp) now take the
// list's +1 — but completed the RELEASE side only where it was provably paired
// (`registerUsingDisposal`). That asymmetry was deliberate: adding a retain can
// only turn a double-free into a bounded leak, while adding a release whose
// construction half is unretained does the reverse. It left every other
// destruction/removal path passing a NULL element disposer, so each LEAKED one
// reference per element.
//
// THE PAIRING PROOF that licenses the releases this test pins. The five insert
// paths above are the ONLY ways an element enters a `bz_llist_t`: the runtime
// (src/runtime/bezel_runtime_collections.cpp) exposes no other insert — its
// `bz_llist_add_before`/`_after` delegate to `_first`/`_last` and nothing else
// in the runtime calls them — and codegen has exactly two `bz_llist_alloc`
// sites, that literal arm and `new LinkedList<T>()` (which allocates EMPTY). So
// every element of a borrowing `LinkedList<string>` carries exactly one
// list-owned reference (either retained by `llIsStrElem`/`llitIsStrElem`, or a
// fresh +1 TRANSFERRED via `isPlusOneStringProducer`), and a release that runs
// exactly once per element consumes exactly that one.
//
// `.TryPopFirst`/`.TryPopLast` are deliberately NOT given a disposer:
// `bz_llist_pop_first`/`_last` MOVE the element out by value into the caller's
// out-param, transferring the reference rather than dropping it.
//
// ⚠ WHY THE STRING MUST BE BUILT AT RUNTIME. `"ab" + "cd"` constant-folds to an
// RC-SENTINEL string and `bz_string_release` is a NO-OP on a sentinel, so a
// literal-built probe passes whether or not the bug is present. Every program
// below builds its string with interpolation at run time (`$"v-{i}"`).
//
// ⚠ WHY THE SHARED STRING IS A PARAMETER, NOT A LOCAL. A `var s: string := $"…"`
// that escapes into a collection is dropped from `stringReleaseEligible_` (#422)
// and leaks its +1 — PRE-EXISTING and unrelated (measured at 731 B / 40
// allocations on #2686's repro). LSan would otherwise fail these tests for that
// unrelated reason. Passing the string as a borrowed PARAMETER keeps the
// caller's +1 accounted for, so these programs are exactly leak-neutral and
// `runAotChecked` observes ONLY the #2692 balance.
//
// OBSERVABILITY. `runAotChecked` links and runs the generated code under
// ASan/LSan, so BOTH directions flip `r.ran` to false: a LEAK (the bug — a
// missing release) and a double-free / SEGV (an over-correction — a release the
// insert side never paired). The exit code independently pins the element counts
// read back OUT of the lists, so a premature free corrupts it regardless of the
// sanitizer.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// (A) `.RemoveFirst()` / `.RemoveLast()` — LlvmBackend_Calls3.cpp, the
// `isOwning ? llElemPol.disposerThunk : nullptr` sites. Four elements go in (so
// all four insert paths that a plain list can reach are exercised), two come out
// via the removals and two via the `using var` scope-end release #2686 added.
// Pre-fix the two REMOVED elements leak their +1; post-fix the list's four
// references are released exactly four times.
// Value: 20 iterations x 2 remaining = 40.
const char* kRemoveFirstLastReleasesElement =
    "module M; class C "
    "  public static method Four(s: string): int "
    "    using var l: LinkedList<string> := new LinkedList<string>(); "
    "    l.AddLast(s); "
    "    l.AddLast(s); "
    "    l.AddFirst(s); "
    "    l.AddFirst(s); "
    "    l.RemoveFirst(); "
    "    l.RemoveLast(); "
    "    return l.Count; "                          // 2
    "  end Four; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Four($\"v-{i}\"); "   // runtime-built, NOT folded
    "    end; "
    "    return total; "                            // 40
    "  end Main; "
    "end C;";

// (B) `.Clear()` and `.Remove(item)` — the loop-of-remove_first site and the
// find-then-remove_node site. `.Remove` matches by CONTENT equality (#565), so
// the element it unlinks is the list's own +1, not the caller's borrowed
// argument: releasing the argument instead of the stored element would be the
// wrong reference and ASan would see the caller's string die early.
// Value: 20 iterations x (1 removed-flag + 0 left after Clear) = 20.
const char* kClearAndRemoveReleaseElements =
    "module M; class C "
    "  public static method Three(s: string): int "
    "    using var l: LinkedList<string> := new LinkedList<string>(); "
    "    l.AddLast(s); "
    "    l.AddLast(s); "
    "    l.AddLast(s); "
    "    var hit := 0; "
    "    if l.Remove(s) then hit := 1; end; "
    "    l.Clear(); "
    "    return hit + l.Count; "                    // 1
    "  end Three; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Three($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 20
    "  end Main; "
    "end C;";

// (C) `.RemoveNode(node)` — the node-anchored removal. The node handle comes
// back from `.AddLast`, and the element released must be the one INSIDE that
// node. The surviving element's length is read back out of the list afterwards,
// so a release aimed at the wrong reference corrupts the exit code independently
// of the sanitizer.
// Value: 20 iterations x len("v-i"); lengths sum to 71 over 1..20 -> 71.
const char* kRemoveNodeReleasesElement =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    using var l: LinkedList<string> := new LinkedList<string>(); "
    "    var first := l.AddLast(s); "
    "    l.AddLast(s); "
    "    l.RemoveNode(first); "
    "    var n := 0; "
    "    for var it in l do "
    "      n := n + it.Length; "
    "    end; "
    "    return n; "                                // len(s)
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 71
    "  end Main; "
    "end C;";

// (D) An `owned _l: LinkedList<string>` FIELD — both the scope-end disposal
// (emitOwnedFieldDisposals, LlvmBackend_Disposal.cpp) and the drop-on-overwrite
// (lowerOwnedCollectionFieldStore, LlvmBackend_Setup.cpp). Both consult the same
// head-name allowlist, which listed Array/Stack/Queue/IList/IReadOnlyList and
// excluded LinkedList; #2692 adds it. `Fill` puts three references in, `Swap`
// DISPLACES that list (releasing all three) and leaves one, and dropping the
// Holder releases that last one.
// Value: 20 iterations x (3 + 1) = 80.
const char* kOwnedLinkedListFieldReleasesElements =
    "module M; "
    "class Holder "
    "  private owned _l: LinkedList<string> := new LinkedList<string>(); "
    "  public method Fill(s: string): int "
    "    _l.AddLast(s); "
    "    _l.AddLast(s); "
    "    _l.AddFirst(s); "
    "    return _l.Count; "                         // 3
    "  end Fill; "
    "  public method Swap(s: string): int "
    "    _l := new LinkedList<string>(); "          // drop-on-overwrite
    "    _l.AddLast(s); "
    "    return _l.Count; "                         // 1
    "  end Swap; "
    "end Holder; "
    "class C "
    "  public static method Round(s: string): int "
    "    using var h := new Holder(); "
    "    var a := h.Fill(s); "
    "    var b := h.Swap(s); "
    "    return a + b; "                            // 4
    "  end Round; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Round($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 80
    "  end Main; "
    "end C;";

// (E) The #2660 INTERIOR literal-tree node — `collectionNodeFreeThunk` in
// LlvmBackend_Disposal.cpp. #2660 had to free an interior `LinkedList<string>`
// node with a NULL child disposer, and its comment said so explicitly: "no
// LinkedList insert path retains, so a release here would still be unpaired".
// #2686 falsified that sentence — the interior node is materialized by the very
// same `materializeHeapCollectionLiteral` arm that now retains — so #2692
// reverts the exclusion exactly as #2682 reverted the dictionary half.
//
// Shape note: the root is a plain `var`, and the counts are read back through
// the INDEXER, matching #2660's own `array_of_arrays_interior_reclaim_looped`
// witness. That is not cosmetic — a `using var` root iterated with a nested
// `for var row in rows` is NOT in #2660's `literalTreeAggregateEligible_` set,
// so no interior disposer is installed at all and the interior `bz_llist_t`
// nodes themselves leak (measured: 4571 B / 140 allocations, dominated by
// `llist_node_alloc` under `bz_llist_add_last`). That is a separate,
// larger, pre-existing #2660 eligibility gap, filed separately — it would mask
// this test rather than witness it.
// Value: 20 iterations x (2 + 1) = 60.
const char* kInteriorLinkedListNodeReleasesElements =
    "module M; class C "
    "  public static method Three(s: string): int "
    "    var rows: Array<LinkedList<string>> := [[s, s], [s]]; "
    "    return rows[0].Count + rows[1].Count; "    // 3
    "  end Three; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Three($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 60
    "  end Main; "
    "end C;";

}  // namespace

// §28.11/§28.9 v0.240 — `.RemoveFirst()` / `.RemoveLast()` release the +1 the
// insert path took (LlvmBackend_Calls3.cpp).
BEZEL_TEST(codegen_linkedlist_string_release_2692,
     remove_first_last_releases_element_2692) {
    JitResult r = runAotChecked(kRemoveFirstLastReleasesElement);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// §28.11/§28.9 v0.240 — `.Clear()` and `.Remove(item)` release each removed
// element's +1 (LlvmBackend_Calls3.cpp).
BEZEL_TEST(codegen_linkedlist_string_release_2692,
     clear_and_remove_release_elements_2692) {
    JitResult r = runAotChecked(kClearAndRemoveReleaseElements);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// §28.11/§28.9 v0.240 — `.RemoveNode(node)` releases the element inside the
// unlinked node, and the surviving element stays readable.
BEZEL_TEST(codegen_linkedlist_string_release_2692,
     remove_node_releases_element_2692) {
    JitResult r = runAotChecked(kRemoveNodeReleasesElement);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

// §28.11/§28.9 v0.240 — an `owned LinkedList<string>` field releases its
// elements both on scope-end disposal and on drop-on-overwrite.
BEZEL_TEST(codegen_linkedlist_string_release_2692,
     owned_linkedlist_field_releases_elements_2692) {
    JitResult r = runAotChecked(kOwnedLinkedListFieldReleasesElements);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 80);
}

// §28.11/§28.9/§15.4 v0.240 — an INTERIOR `LinkedList<string>` node of a literal
// tree releases its elements (the #2660 exclusion, reverted).
BEZEL_TEST(codegen_linkedlist_string_release_2692,
     interior_linkedlist_node_releases_elements_2692) {
    JitResult r = runAotChecked(kInteriorLinkedListNodeReleasesElements);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}
