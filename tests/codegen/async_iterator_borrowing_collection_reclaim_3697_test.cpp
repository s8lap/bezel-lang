// Codegen / leak — issue #3697 (spec §28.9 "Runtime-managed types", §28.11,
// §30.7 "Async iterators", v0.286): an ASYNC ITERATOR body never re-established
// the borrowing-collection reclaim eligibility family for the function it
// actually lowers into, so every `var` borrowing-`Array<T>` / `Set<T>` /
// `Dictionary<K,V>` LITERAL and every fresh borrowing-array / dict / grouping
// MATERIALIZER declared in an `IAsyncEnumerable<T>` body leaked its whole
// backing on the iterator's normal-completion edge.
//
//     public static async method Gen(): IAsyncEnumerable<int>
//       var xs: Array<int> := [10, 20, 30];
//       yield return xs[0]; yield return xs[1]; yield return xs.Count;
//     end Gen;
//
//     $ bezelc --sanitize=address -o p p.bzl && ./p    # 200 full drains
//     total=6600
//     Direct leak of 11200 byte(s) in 200 object(s) ... bz_array_alloc
//     SUMMARY: AddressSanitizer: 24000 byte(s) leaked in 400 allocation(s).
//
// The SYNC-iterator twin of every row here is clean, which is what localises
// the defect to the async-iterator resume fn.
//
// ── WHAT WAS WRONG. Not a pass that mis-answered: the pass never ran.
// The five registration sites in `lowerLocalDecl` gate on
// `currentFn_ == borrowingArrayFreeEligibleFn_` (and the dict-materializer,
// grouping, set-literal and dict-literal analogues). An async-iterator body
// lowers with `currentFn_ == resumeFn`, while `emitFunctionBody`
// (LlvmBackend_Setup.cpp) left all five keyed to the ENTRY (factory) fn — so
// every gate was false for the whole body and every reclaim was skipped.
//
// This is the SIXTH member of `emitAsyncIteratorMethod`'s "re-establish this
// pass for the resume fn" family, and the last one it was missing:
// #1896/#1897 (unwind ineligibility), #2817 (string release), #3466 (cell
// frame), narrow #3553 (closure release), fix #3507 (capture-cell free) — and
// now this one. Its three sibling emitters have carried it for a year:
// `emitIteratorMethod` since fix #2148, `emitAsyncMethod` since #2132/#2142/
// #2143, `emitAsyncLambda` alongside. `emitAsyncIteratorMethod` never did.
//
// ── DIRECTION. This ARMS five reclaims, so the risk is an OVER-RELEASE, not a
// leak. Every row is `runAotChecked` (generated-code ASan with LeakSanitizer
// on), so a double free or a use-after-free fails the row before the printed
// value is compared, and the printed value is itself a corruption oracle. The
// two edges where an iterator's dispose stack could run a SECOND time are
// rows 8 and 9: `break` out of `await for` (§29.7 abandonment, where the yield
// handler's `smIterCleanupAtState_` snapshot replays the stack from Dispose)
// and a body that THROWS after a yield. Both were measured clean at 200
// activations. Row 10 is the save/restore probe for the same reason.
// per §28.9, §28.11, §30.7 (issue #3697)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a leak, a double free or a use-after-free fails the row before the
// printed value is compared. Every leak row here is loop-amplified to 200
// activations: a one-shot row reads clean on BOTH sides, because the single
// allocation is still stack-reachable at exit.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

const char* kHdr =
    "module M; import System; import System.Collections.Generic; "
    "import System.Threading.Tasks; ";

// The 200-round async driver shared by rows 1-6: full drain, then a trailing
// statement so a leaked backing is LSan-visible (LSan is a reachability
// detector; without a trailer a dead register can still keep it reachable).
const char* kDrain =
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var j in 0..<200 do "
    "      await for var v in C.Gen() do total := total + v; end; "
    "    end; "
    "    Console.WriteLine(\"total=\" + total.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";
}  // namespace

// ── Row 1, the issue's repro: an `Array<int>` LITERAL local held live across
// the yields. Pre-fix 24000 B / 400 allocations from `bz_array_alloc`; the
// printed total is unaffected either way, so LSan is the ONLY oracle that sees
// this defect and this row must not be weakened to a plain value check.
// 200 * (10 + 20 + 3) = 6600. // per §28.9, §28.11 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           array_literal_in_an_async_iterator_reclaims_its_backing_spec_28_11) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var xs: Array<int> := [10, 20, 30]; "
        "    yield return xs[0]; "
        "    yield return xs[1]; "
        "    yield return xs.Count; "
        "  end Gen; " + kDrain),
        "total=6600\n");
}

// ── Row 2, the STRING-DISPOSER axis: `"a,b,c,d".Split(",")` is a fresh
// borrowing `Array<string>` MATERIALIZER whose reclaim carries a NON-null
// per-element disposer (the string-release thunk), unlike row 1's null one —
// so the backing AND all four substrings leaked. Pre-fix 51200 B / 2000
// allocations. 200 * (1 + 4) = 1000. // per §28.9 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           split_materializer_in_an_async_iterator_reclaims_substrings_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var parts := \"a,b,c,d\".Split(\",\"); "
        "    yield return 1; "
        "    yield return parts.Count; "
        "  end Gen; " + kDrain),
        "total=1000\n");
}

// ── Row 3, the DICT-MATERIALIZER axis (#1028): `nums.ToDictionary(sel)` is a
// fresh `owned Dictionary<K,V>` the local solely owns, gated on
// `dictMaterializerFreeEligibleFn_`. Both the `nums` literal AND the dict must
// reclaim. Pre-fix 109600 B / 1600 allocations — the largest of the six.
// 200 * (1 + 3) = 800. // per §28.9, §28.11 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           dict_materializer_in_an_async_iterator_reclaims_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var nums: Array<int> := [1, 2, 3]; "
        "    var d := nums.ToDictionary(lambda(n: int) => n); "
        "    yield return 1; "
        "    yield return d.Count; "
        "  end Gen; " + kDrain),
        "total=800\n");
}

// ── Row 4, the SET-LITERAL axis (#2142): a `Set<int>` literal's backing table.
// Pre-fix 85600 B / 1200 allocations. 200 * (1 + 3) = 800.
// per §28.9, §15.4 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           set_literal_in_an_async_iterator_reclaims_its_table_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var s: Set<int> := [10, 20, 30]; "
        "    yield return 1; "
        "    yield return s.Count; "
        "  end Gen; " + kDrain),
        "total=800\n");
}

// ── Row 5, the DICT-LITERAL axis (#2143): a `Dictionary<string, int>` literal,
// whose reclaim is additionally the staleness guard #2660 reuses. Pre-fix
// 84000 B / 1000 allocations, the string keys among them.
// 200 * (1 + 2) = 600. // per §28.9, §15.4 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           dict_literal_in_an_async_iterator_reclaims_its_table_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var d: Dictionary<string, int> := [\"a\": 10, \"b\": 20]; "
        "    yield return 1; "
        "    yield return d.Count; "
        "  end Gen; " + kDrain),
        "total=600\n");
}

// ── Row 6, the GROUPING axis (#1930): a bare `GroupBy(sel).First()` returns a
// grouping object the local owns, gated on `groupingFreeEligibleFn_`. Pre-fix
// 44000 B / 1200 allocations. 200 * (1 + 2) = 600.
// per §28.9, stdlib §5 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           groupby_terminal_in_an_async_iterator_reclaims_the_grouping_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var xs: Array<int> := [1, 2, 3, 4]; "
        "    var g := xs.GroupBy(lambda(x: int) => x mod 2).First(); "
        "    yield return 1; "
        "    yield return g.Count(); "
        "  end Gen; " + kDrain),
        "total=600\n");
}

// ── Row 7, the SYNC-ITERATOR twin of row 1: the byte-identical body through
// `emitIteratorMethod`, which has had this block since fix #2148. Clean on
// both sides; this row is the one that says the remedy is parity with an
// existing site rather than a new rule, and that this commit did not disturb
// it. // per §28.11 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           sync_iterator_twin_of_the_array_literal_body_stays_clean_spec_28_11) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var xs: Array<int> := [10, 20, 30]; "
        "    yield return xs[0]; "
        "    yield return xs[1]; "
        "    yield return xs.Count; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      for var v in C.Gen() do total := total + v; end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=6600\n");
}

// ── Row 8, OVER-RELEASE probe 1 of 2 — §29.7 ABANDONMENT. The consumer
// `break`s after the first element, so the iterator is Disposed while
// suspended with the literal live in its frame, and the yield handler's
// `smIterCleanupAtState_` snapshot replays this commit's newly-armed reclaim
// from Dispose. It must run EXACTLY once over 200 abandonments: a second run
// is an ASan double-free, none is an LSan leak, and `runAotChecked` fails the
// row on either. 200 * 10 = 2000. // per §29.7, §28.11 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           abandoned_async_iterator_reclaims_the_literal_exactly_once_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var xs: Array<int> := [10, 20, 30]; "
        "    yield return xs[0]; "
        "    yield return xs[1]; "
        "    yield return xs[2]; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      await for var v in C.Gen() do total := total + v; break; end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2000\n");
}

// ── Row 9, OVER-RELEASE probe 2 of 2 — THROW AFTER A YIELD. The body throws
// once an element has been produced, so the exception edge runs the dispose
// stack AND the consumer's unwind reaches Dispose. The same exactly-once
// obligation as row 8, on the edge the #3500 comment in LlvmBackend_Stmts.cpp
// records as historically the dangerous one. 200 * (10 + 1) = 2200.
// per §29.7, §30.7 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           throwing_async_iterator_reclaims_the_literal_exactly_once_spec_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var xs: Array<int> := [10, 20, 30]; "
        "    yield return xs[0]; "
        "    throw new InvalidOperationException(\"pqr\"); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        await for var v in C.Gen() do total := total + v; end; "
        "      catch "
        "        var e: Exception => do total := total + 1; end "
        "      end try; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2200\n");
}

// ── Row 10, the SAVE/RESTORE half. `emitAsyncIteratorMethod` is reached
// mid-emission of an OUTER body by `instantiateGenericSpecialization`, whose
// `SmFrameSaveScope` does not cover these six eligibility sets or their five
// function tokens. Here a GENERIC async iterator is specialised from inside
// `Outer`, and `Outer` then declares an array literal of its OWN. Without the
// `BorrowingReclaimSetScope` this commit adds beside `CaptureReclaimSetScope`,
// `borrowingArrayFreeEligibleFn_` is left pointing at the specialisation's
// resumeFn and `ys` is silently skipped — a leak in exactly the direction this
// commit exists to close. MEASURED, with the scope line commented out and
// everything else of this commit in place: 24000 B / 400 allocations, all of
// it `Outer`'s own `ys` backing (the specialisation's own `zs` still
// reclaims). So this row fails without the scope, not only without the
// establishment.
// 200 * ((2 + 7) + 3) = 2400. // per §28.11, §30.7 (issue #3697)
BEZEL_TEST(async_iterator_borrowing_collection_reclaim_3697,
           generic_async_iterator_spec_restores_the_outer_bodys_tokens_spec_28_11) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static async method Gen<T>(a: T, b: T): IAsyncEnumerable<int> "
        "    var zs: Array<T> := [a, b]; "
        "    yield return zs.Count; "
        "    yield return 7; "
        "  end Gen; "
        "  public static async method Outer(): Task<int> "
        "    var t := 0; "
        "    await for var v in C.Gen<int>(1, 2) do t := t + v; end; "
        "    var ys: Array<int> := [5, 6, 7]; "
        "    return t + ys.Count; "
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + await C.Outer(); end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2400\n");
}
