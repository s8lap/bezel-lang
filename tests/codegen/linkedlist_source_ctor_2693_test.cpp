// Codegen — the documented `new LinkedList<T>(source)` constructor silently
// ignored its argument and produced an EMPTY list. Issue #2693.
//
// Spec: stdlib §4.6 "`LinkedList<T>`" declares exactly two constructors —
//
//     public constructor();
//     public constructor(source: IEnumerable<T>);
//
// — and stdlib §5 recommends the second BY NAME: "There is no `ToList()`
// because Bezel does not have a dynamic-array type named `List<T>` (`Array<T>`
// is the resizable form; `LinkedList<T>` is the linked form; use
// `new LinkedList<T>(source)` if a linked list is wanted)."  §4.6 also fixes the
// orientation for the sibling literal form — "textual order is head-to-tail" —
// which the source form follows: source element 0 becomes `First`.
// Stdlib revision v0.240.
//
// THE BUG. `lowerNew`'s LinkedList arm (LlvmBackend_Exprs2.cpp) emitted a bare
// `bz_llist_alloc` and never read `ne.args`, so the source constructor compiled,
// ran, and quietly produced zero elements where §4.6 promises the source's. No
// diagnostic, no ICE — a silent wrong value on a documented API.
//
// THE FIX. Drain the source once through `lowerToLazyEnumerator` (the one
// backing-agnostic normalizer: Array/OwningArray/Stack/Queue/OwningBuffer,
// LinkedList, Set/Dictionary, Range, a lazy LINQ pipeline, and a vtable-backed
// `IEnumerable<T>`) into a snapshot array, then append in source order with
// `bz_llist_add_last`.
//
// ⚠ OWNERSHIP — this is a SIXTH insert path into a `bz_llist_t`. #2686 added the
// element RETAIN at the five existing paths and #2692 added the paired RELEASE,
// on the stated invariant that those five are the only ways an element enters
// the list. A populating constructor that stored `string` elements WITHOUT
// `emitStringRetain` would reintroduce exactly the defect #2686 fixed — an ASan
// SEGV in `bz_string_release` under `bz$strElemRelease` under `bz_llist_free`.
// `bz_lazy_to_array` does NOT retain (the `ToArray` terminal has to call
// `bz_array_retain_strings` separately), so the snapshot holds BORROWED
// references and the constructor retains per element as it appends.
//
// ⚠ WHY THE OWNERSHIP PROBE BUILDS ITS STRING AT RUNTIME. `"ab" + "cd"`
// constant-folds to an RC-SENTINEL on which `bz_string_release` is a NO-OP, so a
// literal-built probe passes whether or not the balance is right. Test (B)
// builds its string with interpolation at run time (`$"v-{i}"`).
//
// ⚠ WHY THAT STRING IS A PARAMETER, NOT A LOCAL. A `var s: string := $"…"` that
// escapes into a collection is dropped from `stringReleaseEligible_` (#422) and
// leaks its +1 — PRE-EXISTING and unrelated to #2693. Passing it as a borrowed
// PARAMETER keeps the caller's +1 accounted for, so (B) is exactly leak-neutral
// and `runAotChecked` observes ONLY this constructor's balance. Test (C) uses
// string LITERALS on purpose: sentinels make refcounting a no-op there, so (C)
// isolates CONTENTS and ORDER from ownership.
//
// OBSERVABILITY. (B) runs under ASan/LSan, so BOTH directions flip `r.ran`: a
// double-free (no retain — the regression #2686 guarded) and a leak (retaining
// something the list never releases). Every other test runs the PLAIN non-ASan
// path, because `--full` has masked live defects before (#2633/#2629).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(keyword) != std::string::npos)
            return true;
    return false;
}

// (A) The #2693 repro. An `Array<int>` source of three elements. The positional
// encoding `acc := acc * 10 + it` pins the COUNT, every element's VALUE, and the
// head-to-tail ORDER in one number — 123 is reachable only from exactly [1,2,3]
// in that order. Guarded on `Count = 3` so the constructor cannot pass by
// producing a longer list whose encoding happens to collide. Pre-fix the list
// was empty: `Count` was 0, so this returned 0.
const char* kArraySourceCountOrderAndValues =
    "module M; class C "
    "  public static method Enc(l: LinkedList<int>): int "
    "    var acc := 0; "
    "    for var it in l do acc := acc * 10 + it; end; "
    "    return acc; "
    "  end Enc; "
    "  public static method Main(): int "
    "    using var a: Array<int> := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); "
    "    using var l: LinkedList<int> := new LinkedList<int>(a); "
    "    if l.Count = 3 then return C.Enc(l); end; "
    "    return 0; "                                 // 123
    "  end Main; "
    "end C;";

// (B) THE OWNERSHIP BALANCE. A `LinkedList<string>` built from a source must take
// the same +1 per element the other five insert paths take (#2686), and the
// `using var` drop must release exactly that one (#2692). The same borrowed
// string occupies all three slots of the source, so an unpaired release frees it
// out from under the next read; 20 iterations make that certain rather than
// tolerated once. LSan simultaneously catches the over-correction (a retain the
// list never releases). The exit code independently pins that all three elements
// are still readable OUT of the copy.
// Value: 20 iterations x 3 x len($"v-{i}"); len sums to 71 over 1..20 → 213.
const char* kStringElementsRetainedAndReleasedExactlyOnce =
    "module M; class C "
    "  public static method Build(s: string): int "
    "    using var src: Array<string> := new Array<string>(); "
    "    src.Add(s); src.Add(s); src.Add(s); "
    "    using var l: LinkedList<string> := new LinkedList<string>(src); "
    "    var n := 0; "
    "    for var it in l do n := n + it.Length; end; "
    "    return n; "
    "  end Build; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Build($\"v-{i}\"); "   // runtime-built, NOT folded
    "    end; "
    "    return total; "                             // 213
    "  end Main; "
    "end C;";

// (C) STRING CONTENTS AND ORDER, isolated from ownership (literals are RC
// sentinels). The three source elements have distinct lengths, so
// `acc * 10 + it.Length` = 123 pins the order head-to-tail; `Contains` then pins
// that the copy holds the actual CHARACTERS, not merely three strings of the
// right sizes. Returns 123 only when both hold.
const char* kStringSourceContentsAndOrder =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a: Array<string> := new Array<string>(); "
    "    a.Add(\"p\"); a.Add(\"qq\"); a.Add(\"rrr\"); "
    "    using var l: LinkedList<string> := new LinkedList<string>(a); "
    "    var acc := 0; "
    "    for var it in l do acc := acc * 10 + it.Length; end; "
    "    if l.Contains(\"p\") and l.Contains(\"qq\") and l.Contains(\"rrr\") "
    "    then return acc; end; "                     // 123
    "    return 0; "
    "  end Main; "
    "end C;";

// (D) The source may be ANY `IEnumerable<T>` (§4.6), not just an `Array<T>`, so
// the fix must not be a single-backing special case. Bitmask over four backings
// plus the no-argument constructor, which must keep allocating an EMPTY list:
//   1 = Array<int> source count      2 = its order
//   4 = LinkedList<int> source (bz_llist backing, via bz_lazy_from_llist)
//   8 = lazy LINQ pipeline source (Where)
//  16 = empty source → Count 0      32 = `new LinkedList<int>()` still empty
// All five bits → 63. Pre-fix every source-built list was empty, so bits 1/2/4/8
// were all clear and this returned 16+32 = 48.
const char* kSourceAcrossBackings =
    "module M; class C "
    "  public static method Enc(l: LinkedList<int>): int "
    "    var acc := 0; "
    "    for var it in l do acc := acc * 10 + it; end; "
    "    return acc; "
    "  end Enc; "
    "  public static method Main(): int "
    "    var ok := 0; "
    "    using var a: Array<int> := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); "
    "    using var fromArr: LinkedList<int> := new LinkedList<int>(a); "
    "    if fromArr.Count = 3 then ok := ok + 1; end; "
    "    if C.Enc(fromArr) = 123 then ok := ok + 2; end; "
    "    using var fromLl: LinkedList<int> := new LinkedList<int>(fromArr); "
    "    if C.Enc(fromLl) = 123 then ok := ok + 4; end; "
    "    using var fromLazy: LinkedList<int> := "
    "        new LinkedList<int>(a.Where(lambda(x: int) => x > 1)); "
    "    if C.Enc(fromLazy) = 23 then ok := ok + 8; end; "
    "    using var e: Array<int> := new Array<int>(); "
    "    using var fromEmpty: LinkedList<int> := new LinkedList<int>(e); "
    "    if fromEmpty.Count = 0 then ok := ok + 16; end; "
    "    using var noArg: LinkedList<int> := new LinkedList<int>(); "
    "    if noArg.Count = 0 then ok := ok + 32; end; "
    "    return ok; "                                // 63
    "  end Main; "
    "end C;";

// (E) §4.7 declares ONLY `constructor()` on `OwningLinkedList<T>`. The source
// form is therefore NOT specified there and must be REJECTED rather than
// invented: copying elements out of a borrowed `IEnumerable<T>` into an OWNING
// list would have to either alias them (two owners → double dispose) or consume
// the borrow, and the stdlib document answers neither. Pre-fix this compiled and
// silently produced an empty list.
const char* kOwningLinkedListSourceRejected =
    "module M; class W public x: int := 0; end W; "
    "class C "
    "  public static method Main(): int "
    "    using var src: OwningLinkedList<W> := new OwningLinkedList<W>(); "
    "    using var bad: OwningLinkedList<W> := new OwningLinkedList<W>(src); "
    "    return bad.Count; "
    "  end Main; "
    "end C;";

// (F) The builtin collections have no `TypeSymbol`, so sema's `checkNewExprArgs`
// never runs for them and an ill-typed source reaches codegen. `new
// LinkedList<int>(42)` must be diagnosed, not read at a wrong stride and not
// silently returned as the empty list it used to be.
const char* kNonEnumerableSourceRejected =
    "module M; class C "
    "  public static method Main(): int "
    "    using var bad: LinkedList<int> := new LinkedList<int>(42); "
    "    return bad.Count; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #2693 repro — the source constructor copies the source's elements.
// stdlib §4.6 `public constructor(source: IEnumerable<T>)`.
// =====================================================================
BEZEL_TEST(codegen_linkedlist_source_ctor_2693,
           array_source_copies_count_order_and_values_stdlib_4_6) {
    JitResult r = runAot(kArraySourceCountOrderAndValues);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 123);   // pre-fix: 0 (the list was EMPTY)
}

// =====================================================================
// (B) The sixth insert path takes and releases exactly one reference per
// `string` element. stdlib §4.6 / spec §28.11 / §28.9 (#2686 / #2692).
// =====================================================================
BEZEL_TEST(codegen_linkedlist_source_ctor_2693,
           string_elements_balanced_exactly_once_spec_28_11) {
    JitResult r = runAotChecked(kStringElementsRetainedAndReleasedExactlyOnce);
    BEZEL_EXPECT_TRUE(r.ran);           // no double-free AND no leak
    BEZEL_EXPECT_EQ(r.exitCode, 213);   // every copied element still readable
}

// =====================================================================
// (C) The copy holds the source's CHARACTERS, in head-to-tail order.
// stdlib §4.6 ("textual order is head-to-tail").
// =====================================================================
BEZEL_TEST(codegen_linkedlist_source_ctor_2693,
           string_source_contents_and_order_stdlib_4_6) {
    JitResult r = runAot(kStringSourceContentsAndOrder);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 123);   // pre-fix: 0
}

// =====================================================================
// (D) Any `IEnumerable<T>` backing works, and the no-arg form still
// allocates empty. stdlib §4.6.
// =====================================================================
BEZEL_TEST(codegen_linkedlist_source_ctor_2693,
           source_across_backings_stdlib_4_6) {
    JitResult r = runAot(kSourceAcrossBackings);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 63);    // pre-fix: 48 (only the two empty cases)
}

// =====================================================================
// (E) `OwningLinkedList<T>` declares no source constructor — §4.7.
// =====================================================================
BEZEL_TEST(codegen_linkedlist_source_ctor_2693,
           owning_linkedlist_source_ctor_rejected_stdlib_4_7) {
    BEZEL_EXPECT_TRUE(
        hasError(kOwningLinkedListSourceRejected, "no such constructor"));
}

// =====================================================================
// (F) A non-enumerable source is diagnosed, not silently dropped. §4.6.
// =====================================================================
BEZEL_TEST(codegen_linkedlist_source_ctor_2693,
           non_enumerable_source_rejected_stdlib_4_6) {
    BEZEL_EXPECT_TRUE(
        hasError(kNonEnumerableSourceRejected, "requires an enumerable source"));
}
