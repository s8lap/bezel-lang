// Codegen / leak — fix #3384 (§28.6 Leak prevention / §29.2 Desugaring of
// `for` / §28.3 Local variables, ownership, and borrows, v0.283): the
// DECL-SITE enumerable-local wrapper frame was minted and never claimed, so it
// leaked once per execution of the declaration — at BOTH the `IEnumerable<T>`
// and the `IOwningEnumerable<T>` head.
//
// THE DEFECT — a duplicated classification predicate that drifted.
// `Emitter::lowerLocalDecl` (LlvmBackend_Setup.cpp) is the WRAP CREATOR for an
// annotated enumerable local: `var e: IEnumerable<T> := someCollection;` cannot
// keep the raw `bz_array_t*`/`bz_llist*` in its slot, because §29.2's for-in
// desugaring drives an iterator-format vtable ("[0]=td, [1]=Dispose,
// [2]=MoveNext, [3]=Current"), so the coercion `calloc`s a fresh wrapper frame
// over the source. `Emitter::registerIterableSourceDisposal` is the OWNERSHIP
// CLAIM for that frame: it registers the §28.3 scope-end drop that runs the
// frame's slot-1 Dispose. §28.6 is the obligation the pair discharges together
// — nothing else ever frees a wrapper frame, since the source collection's own
// binder disposes the BACKING, not the wrap.
//
// The two sites each spelled the admissible (declared head x initializer
// family) grid INDEPENDENTLY, and the spellings had drifted apart:
//
//   wrap creator  : head IEnumerable OR IOwningEnumerable, over
//                   Array/OwningArray/OwningBuffer/Stack/OwningStack/
//                   Queue/OwningQueue (#1177), Set/OwningSet (#1069),
//                   (Owning)LinkedList (#1177), a lazy LINQ pipeline (#1911),
//                   a user class (#552).
//   ownership claim: head IEnumerable ONLY, over `Array`, `Set`/`OwningSet`,
//                   or a declaration-carrying initializer.
//
// Every mint outside that narrow re-spelling was unclaimed and leaked. This is
// the same defect class as #3392 (the property-read arm of the SAME function)
// and #3355 — "a duplicated classification predicate drifts from the predicate
// it mirrors" — which is why the fix does not add a third spelling.
//
// MEASURED on d05399c (i.e. with #3392, #3385 and #3386 already in), 50
// executions of the declaration, under generated-code ASan + LSan:
//   `var e: IEnumerable<Item> := a;`       over `using var a := new
//       OwningArray<Item>()`          -> 1600 B leaked in 50 allocations
//                                        (32 B per `{vtable, arr, idx, esz}`
//                                        borrowing frame)
//   `var e: IOwningEnumerable<Item> := a;` (same source)
//                                     -> 1200 B leaked in 50 allocations
//                                        (24 B per `{vtable, arr, pending}`
//                                        draining frame — #3385 routes this
//                                        head to `wrapArrayAsIOwningEnumerator`)
//   `var e: IEnumerable<int> := s;` over a `Stack<int>`, a `Queue<int>` and a
//       `LinkedList<int>`             -> 1600 B leaked in 50 allocations each
//   `var e: IEnumerable<int> := a;` over an `Array<int>` and over a `Set<int>`
//       — the two families the stale re-spelling DID admit
//                                     -> clean, which is what identified the
//                                        drift as a positive-list gap rather
//                                        than a missing mechanism.
//
// THE FIX. The claim asks the wrap creator what it actually did instead of
// re-deriving it. #2975 already had `lowerLocalDecl` self-report every frame it
// minted for a decl into `iterFormatWrappedDecls_` (`v != preIterWrap` — the
// coercion's own observation, which no positive list can drift from), and
// `lowerStmt`'s LocalDecl arm runs that coercion IMMEDIATELY before the
// registration, so the report is exact and current at the claim site. The
// report is now also CLEARED when the coercion declines, so a decl body lowered
// once per generic instantiation cannot inherit an earlier instantiation's
// WRAPPED verdict.
//
// CLAIM SUBSET-OF MINT (the no-double-free direction, which is strictly worse
// than the leak being fixed). The set is written at exactly one site under the
// coercion's own observation, so the claim set is now equal to — hence a subset
// of — the mint set by construction. And the Dispose each claim registers is
// the FRAME's own: the borrowing wraps free only `self`, and
// `wrapArrayAsIOwningEnumerator` frees `self` plus an advanced-but-untaken
// pending it owns per §29.1. None of them frees the source backing, so no
// source binder is double-freed. Cases (B), (B2) and (F) below pin that
// directly: the source collection is still intact, still iterable, and still
// disposed exactly once after the views have been dropped.
//
// FAIL-WITHOUT / PASS-WITH, measured by reverting only
// `src/codegen/LlvmBackend_Setup.cpp` and rebuilding: (A) 1632 B / 51 allocs,
// (B) 1200 B / 50, (B2) 24 B / 1, (C) 1632 B / 51, (D) 1632 B / 51 — all
// leak-free with the fix, at the exit codes asserted below. (E) is a CONTROL:
// it is clean on both sides, and pins that the families the stale re-spelling
// did admit keep their claim after the rewrite.
//
// ORACLE. `runAotChecked` compiles the generated program with AddressSanitizer
// and LeakSanitizer enabled, so `r.ran` is simultaneously a no-leak and a
// no-double-free assertion; the exit code carries the ownership observable.
//
// (Bezel block comments are `(* ... *)`; a C++ `//` inside these string
// literals would be lexed as Bezel source.)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

const char* const kItem =
    "module M; "
    "class Item "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item; ";

}  // namespace

// ---------------------------------------------------------------------------
// (A) The `IEnumerable<T>` head over an `OwningArray<T>` named source — the
//     issue's repro. 50 wrapper frames minted, plus a 51st that is iterated.
//     per §28.6 (leak prevention), §29.2 (the vtable protocol the frame
//     implements), §28.3 (the scope-end drop that discharges it)
// ---------------------------------------------------------------------------
BEZEL_TEST(enumerable_decl_site_wrapper_claim_3384,
           ienumerable_head_over_owningarray_claims_wrapper_frame_28_6) {
    const std::string src =
        std::string(kItem) +
        "class Main "
        "  public static method Run(): int "
        "    using var a := new OwningArray<Item>(); "
        "    a.Add(new Item(3)); "
        "    a.Add(new Item(4)); "
        "    var i := 0; "
        "    while i < 50 do "
        "      var e: IEnumerable<Item> := a; "
        "      i := i + 1; "
        "    end while; "
        "    var total := 0; "
        "    var v: IEnumerable<Item> := a; "
        "    for var x in v do "
        "      total := total + x.V; "
        "    end for; "
        "    return a.Count * 100 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // Pre-fix: LeakSanitizer reports 1632 bytes in 51 allocations.
    BEZEL_EXPECT_TRUE(r.ran);
    // Count == 2 and total == 7: the borrowing wrap is non-destructive
    // (§29.2 — an `IEnumerable<T>`-typed source desugars via the
    // non-destructive `IEnumerator<T>`), so both elements are still in the
    // collection and still live after 51 wraps have been minted AND dropped.
    // That is the claim-subset-of-mint half: the scope-end Dispose freed the
    // FRAMES, never the backing the `using var a` owns.
    BEZEL_EXPECT_EQ(r.exitCode, 207);
}

// ---------------------------------------------------------------------------
// (B) The `IOwningEnumerable<T>` twin — the head that leaks byte-identically,
//     which is what makes #3384 an IEnumerable-FAMILY defect rather than an
//     owning-specific one. A bare declaration advances nothing, so the source
//     must be untouched: the frame's slot-1 Dispose disposes only an
//     advanced-but-untaken pending (§29.1) and frees the frame.
//     per §28.6, §29.1 (the protocol), stdlib §4.13 (owning enumeration
//     interfaces)
// ---------------------------------------------------------------------------
BEZEL_TEST(enumerable_decl_site_wrapper_claim_3384,
           iowningenumerable_head_bare_decl_claims_frame_and_spares_source_29_1) {
    const std::string src =
        std::string(kItem) +
        "class Main "
        "  public static method Run(): int "
        "    using var a := new OwningArray<Item>(); "
        "    a.Add(new Item(3)); "
        "    a.Add(new Item(4)); "
        "    var i := 0; "
        "    while i < 50 do "
        "      var e: IOwningEnumerable<Item> := a; "
        "      i := i + 1; "
        "    end while; "
        "    return a.Count * 10 + 7; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // Pre-fix: LeakSanitizer reports 1200 bytes in 50 allocations.
    BEZEL_EXPECT_TRUE(r.ran);
    // Count == 2: fifty draining views were minted and dropped without a
    // single MoveNext, so nothing left the collection and nothing was freed
    // twice — ASan would abort the run on either.
    BEZEL_EXPECT_EQ(r.exitCode, 27);
}

// ---------------------------------------------------------------------------
// (B2) The DECL-SITE face of #3385's stored-view drain: the claimed
//      `IOwningEnumerable<T>` frame must coexist with a full drain. Element
//      ownership moves to the loop variable (§29.1 TakeCurrent, §29.2's owning
//      desugaring) and the frame's Dispose must free only the frame.
//      per §28.6, §29.1, §29.2, stdlib §4.5 / §4.13
// ---------------------------------------------------------------------------
BEZEL_TEST(enumerable_decl_site_wrapper_claim_3384,
           iowningenumerable_decl_drain_frees_frame_not_elements_29_2) {
    const std::string src =
        std::string(kItem) +
        "class Main "
        "  public static method Run(): int "
        "    using var a := new OwningArray<Item>(); "
        "    a.Add(new Item(3)); "
        "    a.Add(new Item(4)); "
        "    var total := 0; "
        "    var e: IOwningEnumerable<Item> := a; "
        "    for var x in e do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "    end for; "
        "    return a.Count * 100 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // Pre-fix: 24 bytes leaked in 1 allocation (the single unclaimed frame).
    BEZEL_EXPECT_TRUE(r.ran);
    // Count == 0 (stdlib §4.5: "elements leave the collection as they are
    // yielded") and total == 7 — each element discharged exactly once, by the
    // loop variable and not also by the frame's Dispose or the array's own
    // element-disposer pass.
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------------
// (C) `Stack<T>` at the `IEnumerable<T>` head — a family the wrap creator has
//     admitted since #1177 and the stale claim never did. The reverse
//     (top-to-bottom) enumerator is minted here too, so its frame needs the
//     same claim, and iteration order must survive the fix.
//     per §28.6, §29.2, stdlib §4.11 (`Stack<T>` iteration order)
// ---------------------------------------------------------------------------
BEZEL_TEST(enumerable_decl_site_wrapper_claim_3384,
           stack_source_at_ienumerable_head_claims_frame_28_6) {
    const std::string src =
        "module M; "
        "class Main "
        "  public static method Run(): int "
        "    var s := new Stack<int>(); "
        "    s.Push(1); "
        "    s.Push(2); "
        "    s.Push(3); "
        "    var i := 0; "
        "    while i < 50 do "
        "      var e: IEnumerable<int> := s; "
        "      i := i + 1; "
        "    end while; "
        "    var total := 0; "
        "    var first := 0; "
        "    var v: IEnumerable<int> := s; "
        "    for var x in v do "
        "      if first = 0 then first := x; end if; "
        "      total := total + x; "
        "    end for; "
        "    return first * 10 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // Pre-fix: LeakSanitizer reports 1632 bytes in 51 allocations.
    BEZEL_EXPECT_TRUE(r.ran);
    // first == 3 (pop order, stdlib §4.11) and total == 6 — the reverse
    // machinery is unchanged by the ownership claim.
    BEZEL_EXPECT_EQ(r.exitCode, 36);
}

// ---------------------------------------------------------------------------
// (D) `LinkedList<T>` at the `IEnumerable<T>` head — the node-walking wrapper,
//     the third family the wrap creator admits and the stale claim omitted.
//     per §28.6, §29.2, stdlib §4.6 (`LinkedList<T>`)
// ---------------------------------------------------------------------------
BEZEL_TEST(enumerable_decl_site_wrapper_claim_3384,
           linkedlist_source_at_ienumerable_head_claims_frame_28_6) {
    const std::string src =
        "module M; "
        "class Main "
        "  public static method Run(): int "
        "    var l := new LinkedList<int>(); "
        "    l.AddLast(4); "
        "    l.AddLast(5); "
        "    var i := 0; "
        "    while i < 50 do "
        "      var e: IEnumerable<int> := l; "
        "      i := i + 1; "
        "    end while; "
        "    var total := 0; "
        "    var v: IEnumerable<int> := l; "
        "    for var x in v do "
        "      total := total + x; "
        "    end for; "
        "    return l.Count * 100 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // Pre-fix: LeakSanitizer reports 1632 bytes in 51 allocations.
    BEZEL_EXPECT_TRUE(r.ran);
    // Count == 2 (the borrowing node-walk leaves the list intact), total == 9.
    BEZEL_EXPECT_EQ(r.exitCode, 209);
}

// ---------------------------------------------------------------------------
// (E) CONTROL — `Array<T>`, one of the two families the STALE claim already
//     admitted. Clean before AND after the fix: the rewrite replaced the
//     re-derivation with the wrap creator's report, so the claims that already
//     existed must survive it. A regression here would mean the report is a
//     PROPER subset of what the old positive list covered.
//     per §28.6, §29.2
// ---------------------------------------------------------------------------
BEZEL_TEST(enumerable_decl_site_wrapper_claim_3384,
           array_source_keeps_its_pre_existing_claim_28_6) {
    const std::string src =
        "module M; "
        "class Main "
        "  public static method Run(): int "
        "    var a := new Array<int>(4); "
        "    a.Add(6); "
        "    a.Add(7); "
        "    var i := 0; "
        "    while i < 50 do "
        "      var e: IEnumerable<int> := a; "
        "      i := i + 1; "
        "    end while; "
        "    var total := 0; "
        "    var v: IEnumerable<int> := a; "
        "    for var x in v do "
        "      total := total + x; "
        "    end for; "
        "    return a.Count * 100 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 213);
}

// ---------------------------------------------------------------------------
// (F) The no-double-free pin at its sharpest: TWO views over the SAME owning
//     source in one scope, both claimed, both dropped at that scope's end —
//     and the source is then still fully usable and is disposed exactly once
//     by its own `using var`. Under ASan a frame Dispose that reached the
//     backing would abort here (double free / heap-use-after-free) rather than
//     merely mis-count.
//     per §28.3 (the local's affine auto-drop), §28.6, §29.2
// ---------------------------------------------------------------------------
BEZEL_TEST(enumerable_decl_site_wrapper_claim_3384,
           two_claimed_views_over_one_owning_source_free_only_frames_28_3) {
    const std::string src =
        std::string(kItem) +
        "class Main "
        "  public static method Run(): int "
        "    using var a := new OwningArray<Item>(); "
        "    a.Add(new Item(3)); "
        "    a.Add(new Item(4)); "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 20 do "
        "      var e: IEnumerable<Item> := a; "
        "      var f: IEnumerable<Item> := a; "
        "      for var x in f do "
        "        total := total + x.V; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    return a.Count * 100 + (total - 133); "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // Pre-fix this shape LEAKED (two unclaimed frames per iteration) but did
    // not crash; the assertion that matters here is the post-fix one — 40
    // claimed frames dropped, the source untouched.
    BEZEL_EXPECT_TRUE(r.ran);
    // total == 20 * 7 == 140, so `total - 133` == 7, and Count == 2: every
    // element survived 40 wrap-and-drop cycles and 20 borrowing iterations.
    BEZEL_EXPECT_EQ(r.exitCode, 207);
}
