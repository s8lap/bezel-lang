// Codegen / disposal — fix #3354 (spec §28.2 / §28.9 / §28.11, v0.278):
// the `LinkedList<T>` -> `IEnumerable<T>` RETURN COERCION routed the source
// through `wrapLinkedListAsIEnumerable` (the BORROWING wrapper, slot-1
// Dispose = `free(self)` only — the wrapper struct, never the backing
// `bz_llist_t*`) and then called `freeLinkedListSourceNow(v, actualTy)`
// immediately. The wrapper struct carries the source `list` pointer in
// slot 1, and `wrapLinkedListAsIEnumerable`'s `MoveNext` / `Current`
// dereference `bz_llist_first_node` / `bz_llist_node_next` on that pointer.
// Once the immediate free ran, every iteration in the CALLER walked freed
// memory — a textbook use-after-free, confirmed under ASan/LSan.
//
// THE BUG SURFACE. The return-coercion arm in `wrapAsIEnumerableForTarget`
// (LlvmBackend_Assign.cpp) chose the BORROWING wrap for the callee-owned
// case because the sibling OWNING wrap (`wrapOwnedLinkedListAsIEnumerable`,
// added by #2987 for the local-decl-init path) was believed to be safe only
// on EMPTY sources. That belief was correct for the local-decl-init path
// (where the source is `new LinkedList<T>()` by construction), but the
// RETURN coercion hands over a populated list (the callee has called
// `.AddFirst` / `.AddLast` etc., retaining each element), and the immediate
// `bz_llist_free` happens BEFORE the caller iterates. The companion #2710
// test for Array<string> deliberately noted this LinkedList follow-up:
//
//     "DELIBERATELY NOT PINNED HERE. The LinkedList return-coercion path
//      ... is a pre-existing UAF: the BORROWING wrapper stores the source
//      `list` pointer in slot 1, and the caller's iteration walks
//      `bz_llist_first_node` / `bz_llist_node_next` on the SAME backing
//      that `freeLinkedListSourceNow` freed immediately after the wrap was
//      returned."
//
// THE FIX. Route the callee-owned LinkedList return-coercion through the
// OWNING sibling `wrapOwnedLinkedListAsIEnumerable` (the same wrap the
// local-decl-init path uses). Its slot-1 Dispose frees the backing
// `bz_llist_t*` AND the wrapper struct in one call, deferred to the
// CALLER's scope end via the registered iterEnumDispose. The caller's
// `for x in e do ... end` walks a live list, sums correctly, and only then
// the wrapper's slot-1 Dispose runs — exactly once, no UAF, no leak.
// GetEnumerator (slot 4) stamps fresh NON-owning clones with the PLAIN
// vtable, so a per-loop enumerator disposes only its own struct (#2987's
// re-iterability contract). This is the LinkedList analogue of the #2299
// Array/`wrapOwnedArrayAsIEnumerable` fix.
//
// OBSERVABILITY. `runAotChecked` links and runs the generated code under
// ASan/LSan, so a UAF flips `r.ran` to false. The exit code independently
// pins the sum read OUT of the IEnumerable. Integer elements are used (no
// retain-count coupling to the element-slot table), so the test isolates
// §28.9 / §28.11's "defer disposal until after iteration" rule from #2710's
// element-disposer rule. The UAF is on `bz_llist_first_node` of the FREED
// backing, which ASan catches regardless of element type.
//
// WHY THIS EXIT CODE LIVES IN STDOUT. The pre-fix compiler crashes inside
// `bz_llist_first_node` (use-after-free on freed `bz_llist_t*`); the
// process exits with a sanitizer status, not the program's `return 0`.
// The observable moves to stdout (`HITS <sum>`) so the post-fix assertion
// is robust against the 8-bit WEXITSTATUS truncation (aot_support.hpp
// "8-bit exit truncation" note).

#include <cstdio>
#include <cstdlib>
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

// Parse a trailing "HITS <number>" line from `r.stdoutText`.
long parseHits(const std::string& text) {
    auto pos = text.rfind("HITS ");
    if (pos == std::string::npos) return -1;
    return std::strtol(text.c_str() + pos + 5, nullptr, 10);
}

// ── (A) The #3354 repro: `LinkedList<int>` returned as `IEnumerable<int>` ────
//
// The minimal return-coercion shape: a method declares its return type as
// `IEnumerable<int>`, allocates a `LinkedList<int>`, adds elements, and
// returns it. The wrap site coerces the `LinkedList<int>` to `IEnumerable<int>`.
// Pre-fix: backing freed at the return-coercion site; caller's `for x in e`
// UAFs on `bz_llist_first_node` of the freed list — ASan SEGV. Post-fix:
// backing held by the OWNING wrapper until the caller's scope end; iteration
// completes; sum read out.
//
// Value: 50 iterations * (1 + 2 + 3) = 300 (fits 8-bit).
const char* kLinkedListReturnAsIEnumerable =
    "module M; import System; "
    "class C "
    "  public static method Three(): IEnumerable<int> "
    "    var l := new LinkedList<int>(); "
    "    l.AddLast(1); "
    "    l.AddLast(2); "
    "    l.AddLast(3); "
    "    return l; "                                 // ← the bug site
    "  end Three; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 50 do "
    "      for var it in C.Three() do total := total + it; end; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine($\"HITS {total}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ── (B) Multi-iteration re-enumeration contract ──────────────────────────────
//
// The OWNING wrap's slot-4 GetEnumerator stamps each fresh clone with the
// PLAIN (borrowing) vtable, so a per-loop enumerator is a fresh iterator
// over the same shared backing. Iterating the same returned IEnumerable
// TWICE in the same scope must yield the same sum twice — the backing is
// stable (held by the OWNING wrapper until the caller's scope end).
//
// Value: 30 iterations * ((1+2+3) + (1+2+3)) = 30 * 12 = 360 (fits 16-bit).
const char* kLinkedListReturnAsIEnumerableReiterable =
    "module M; import System; "
    "class C "
    "  public static method Three(): IEnumerable<int> "
    "    var l := new LinkedList<int>(); "
    "    l.AddLast(1); "
    "    l.AddLast(2); "
    "    l.AddLast(3); "
    "    return l; "
    "  end Three; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 30 do "
    "      var e := C.Three(); "
    "      for var it in e do total := total + it; end; "
    "      for var it in e do total := total + it; end; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine($\"HITS {total}\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) `LinkedList<int>` returned as `IEnumerable<int>` does NOT UAF on
// iteration. §28.9 / §28.11 / §28.2. The fix applies uniformly to both
// `LinkedList` and `OwningLinkedList` (both match `isLinkedListTypeName`
// at the return-coercion dispatch — see the call site in
// `wrapAsIEnumerableForTarget`); `OwningLinkedList<T>` is reserved for
// class/interface element types (stdlib §4.2 / §14.4, #1327), so the
// borrowing-family `LinkedList<int>` is what exercises this codegen path
// with an integer element.
// =====================================================================
BEZEL_TEST(codegen_owning_linked_list_returned_as_ienumerable_3354,
           linked_list_return_as_ienumerable_no_uaf_28_9) {
    JitResult r = runAotChecked(kLinkedListReturnAsIEnumerable);
    BEZEL_EXPECT_TRUE(r.ran);          // no UAF on bz_llist_first_node
    BEZEL_EXPECT_EQ(parseHits(r.stdoutText), 300);
}

// =====================================================================
// (B) Re-iterating the returned IEnumerable yields the same sum. The
// OWNING wrap's slot-4 GetEnumerator stamps fresh NON-owning clones with
// the PLAIN vtable (§2987's re-iterability contract). §29.11 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_owning_linked_list_returned_as_ienumerable_3354,
           linked_list_return_as_ienumerable_reiterable_29_11) {
    JitResult r = runAotChecked(kLinkedListReturnAsIEnumerableReiterable);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(parseHits(r.stdoutText), 360);
}