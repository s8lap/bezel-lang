// fix #3657 / #3658 (§30.5 "Result lifetime and task disposal" / §28.3, v0.286):
// `Emitter::patternAResultCollectionDisposer` (LlvmBackend_Disposal.cpp)
// decides whether a §30.5 Pattern A task result (`Task<T>`) is an intrinsic
// collection whose backing the task must free at disposal. Its membership
// test used to end in an OPEN-ENDED `common::isOwningPrefixedName(collName)`
// fallback, which matches ANY name starting with "Owning" — including
// `OwningTask`. A `Task<OwningTask<U>>` result therefore entered the
// array-backed free path and was freed with `bz_array_free` on a task
// object, even though this file's own `patternAResultDisposerFor`
// (LlvmBackend_Async.cpp) documents, a few lines past its call into this
// function (MEASURED: sixteen), that a nested Task-family result is BORROWED, not OWNED (fix
// #1889 / #776 / #777) — that guard was unreachable for `OwningTask` because
// the collection branch returned non-null first.
//
// #3638 (`git show 8324364`) had already built the fix's other half: a
// closed membership table, `common::isPatternAResultCollectionHead`
// (src/common/PatternAResultCollection.hpp), which the OWNERSHIP side
// (`taskResultArgIsClass`, src/ownership/Ownership_Checks.cpp) now consults
// instead of a hand-written allowlist. That table's own header comment
// documents the exclusion diff against codegen's *un-fixed* computation —
// `OwningTask` admitted by codegen, excluded from the table on purpose,
// filed as this issue. #3658 is the dedup half: codegen's membership test
// now ALSO reads that one table (kind dispatch — which runtime free fn and
// which per-element disposer family — stays codegen's own, since the table
// answers "is this a member" but not "which of the four free fns applies").
//
// FAIL-WITHOUT / PASS-WITH (measured against this file's own repro,
// `nested_owning_task_result_amplified_200x_3657` below, single-process):
//   pre-fix:  `./bezelc ... --sanitize=address` + run -> AddressSanitizer:
//             DEADLYSIGNAL / SEGV inside `__asan::Allocator::Deallocate`,
//             stack `bz_array_free <- bz$patAResultCollFree$OwningTask <-
//             bz_task_free <- bz$async$M.C$Run$Resume` (exact frames from a
//             1-iteration variant of this program). 5/5 crashing runs.
//   post-fix: LSan-clean, exit=0, stdout the loop's amplified sum. 5/5 clean
//             runs (`scripts/agent_check.sh --probe --asan`).
//
// This file also carries the #3658 "both sides agree" pin: since codegen and
// ownership now read the literal SAME predicate, agreement is structural —
// what can still drift is the TABLE's membership itself, so
// `pattern_a_result_collection_head_membership_snapshot_30_5` below pins the
// full 15-name positive set plus the negatives that matter (the two
// deliberate exclusions `#3638`'s header documents, plus `OwningTask`, the
// #3657 defect this file fixes), mirroring tests/support/
// builtin_registry_snapshot_test.cpp's snapshot idiom for the sibling
// BuiltinRegistry tables.
//
// §6b exclusion diff (every name the OLD codegen-only open-ended fallback
// admitted that the shared table does not, and what handles it now) is
// recorded in full at LlvmBackend_Disposal.cpp's
// `patternAResultCollectionDisposer` header comment; the short version:
//   * `OwningTask`                 -> falls through to the pre-existing
//     `isTaskTypeName(tElem)` guard in `patternAResultDisposerFor`, exactly
//     like `Task<Task<U>>` has always been handled. NOT a new leak: the
//     inner task is BORROWED, its real owner (whoever awaits it) disposes
//     it, per §30.5.
//   * `OwningTaskCompletionSource` -> a SEPARATE, MEASURED, pre-existing gap
//     (LlvmBackend_ElementPolicy.cpp's `elementPolicyFor` has no
//     TaskCompletionSource-family arm), filed as #3685 rather than fixed here
//     (different file; out of #3657/#3658's scope).
//   * `OwningBuffer`, `OwningArray`, etc. (the 15 table members) ->
//     unchanged. `OwningBuffer` specifically has its OWN, separately-filed
//     (#3684), fix-UNRELATED leak (MEASURED identical on this fix and its parent —
//     the `isBuffer` kind-dispatch flag is set by the literal `collName ==
//     "OwningBuffer"` test, independent of the membership gate this fix
//     changes), so it is deliberately NOT one of this file's LSan-clean
//     control rows; see #3684 instead.

#include <array>
#include <string>
#include <string_view>

#include "common/PatternAResultCollection.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// r.ran ? r.exitCode : -999, matching the #2279 sibling file's convention —
// an oracle that CANNOT observe the crash/leak signal is not an oracle, so
// -999 is a sentinel a legitimate program return value cannot produce.
int run(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// #3658: the closed table's full membership, pinned directly (mirrors
// tests/support/builtin_registry_snapshot_test.cpp's style for the sibling
// BuiltinRegistry tables). Both `taskResultArgIsClass` (ownership) and
// `patternAResultCollectionDisposer` (codegen, this fix) now call this ONE
// function, so agreement between the two sides is structural; what a future
// lane could still get wrong is the table's OWN membership, which is what
// this pins.
// ---------------------------------------------------------------------------
BEZEL_TEST(PatternATaskResultCollectionMembership3657_3658,
           pattern_a_result_collection_head_membership_snapshot_30_5) {
    using bezel::common::isPatternAResultCollectionHead;
    // IN: the 15 heads, exactly as declared in
    // common/PatternAResultCollection.hpp's kPatternAResultCollectionHeads.
    static constexpr std::array<std::string_view, 15> kSnapshot = {
        "Array", "OwningArray", "Stack", "OwningStack", "Queue", "OwningQueue",
        "Set", "OwningSet", "SortedSet",
        "Dictionary", "OwningDictionary", "SortedDictionary",
        "LinkedList", "OwningLinkedList",
        "OwningBuffer",
    };
    for (auto n : kSnapshot) BEZEL_EXPECT_TRUE(isPatternAResultCollectionHead(n));

    // OUT: `OwningTask` — the #3657 defect. The open-ended `isOwningPrefixedName`
    // fallback this fix retires would have matched it; the closed table does
    // not, by construction (see the header's own exclusion-diff comment).
    BEZEL_EXPECT_FALSE(isPatternAResultCollectionHead("OwningTask"));
    // OUT: `FrozenArray` — #3638's OTHER deliberate exclusion (§4.19): it is
    // in BuiltinRegistry's array-backed collection table, but codegen's
    // ORIGINAL kind-dispatch list never disposed it, so admitting it here
    // would be a pure false reject with nothing to guard.
    BEZEL_EXPECT_FALSE(isPatternAResultCollectionHead("FrozenArray"));
    // OUT: near-neighbours that must stay OUT for unrelated reasons.
    for (std::string_view n : {"Task", "TaskCompletionSource",
                               "OwningTaskCompletionSource",
                               "IEnumerable", "IOwningEnumerable",
                               "MyClass", "OwningBox", "OwningSortedDictionary",
                               ""})
        BEZEL_EXPECT_FALSE(isPatternAResultCollectionHead(n));
}

// ---------------------------------------------------------------------------
// #3657: `Task<OwningTask<U>>` — the issue's own shape. An async method
// returning `OwningTask<U>` (`C.Produce`), wrapped by an OUTER async method
// declared `Task<OwningTask<U>>` (`C.Wrap`) that simply forwards the call.
// Pre-fix: `C.Wrap()`'s own (unbound, K8-boundary) task frame is disposed
// right after its `await` produces `inner`, and that disposal wrongly
// invokes `bz_array_free` on the `OwningTask<Cell>` — the crash this test
// pins. Post-fix: the outer frame's disposal is a correct no-op for its
// borrowed `OwningTask` result (the #1889/#776/#777 contract, extended).
//
// fix #4670 / §30.5 (v0.307): `C.Wrap(): Task<OwningTask<Cell>>` is a
// Pattern A producer (OwningTask<Cell> is class-like), so `await
// C.Wrap()` produces a BORROW, not an owned value; §30.5's consumer-side
// rules admit that borrow only to a plain `var` (here, the scoped
// await-bind desugaring of `var inner := await C.Wrap();`, since
// `C.Wrap()` is a bare producer call), never to `using var`. This row
// used `using var inner := await C.Wrap();` before #4670 landed; #4670
// correctly rejects that shape now, so it is converted to `var inner`
// here. `inner`'s own await (`using var c := await inner;`) is unaffected
// — `inner: OwningTask<Cell>` is Pattern B, whose canonical discharge IS
// `using var`.
// ---------------------------------------------------------------------------
BEZEL_TEST(PatternATaskResultCollectionMembership3657_3658,
           nested_owning_task_result_use_after_free_3657) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell "
        "  public value: int; "
        "  public constructor(v: int) value := v; end constructor; "
        "end Cell; "
        "class C "
        "  public static async method Produce(): OwningTask<Cell> "
        "    return new Cell(7); "
        "  end Produce; "
        "  public static async method Wrap(): Task<OwningTask<Cell>> "
        "    return C.Produce(); "
        "  end Wrap; "
        "  public static async method Run(): Task<int> "
        "    var inner := await C.Wrap(); "
        "    using var c := await inner; "
        "    return c.value; "
        "  end Run; "
        "  public static method Main(): int "
        "    using var t := C.Run(); "
        "    t.Wait(); "
        "    return t.Result; "
        "  end Main; "
        "end C;"), 7);
}

// LOOP-AMPLIFIED: the produce/wrap/await/dispose cycle runs 200 times in one
// process, each with a distinct value, so a reintroduced wrong-free cannot
// pass by landing in still-mapped memory on one lucky iteration. Expected
// sum: 0+1+...+199 = 19900, printed via `Console.WriteLine` and read back
// from `stdoutText` rather than `exitCode` (the AOT harness's exit code
// truncates to 8 bits, and 19900 does not fit). Pre-fix this crashes on
// iteration 1 (proven above and independently via
// `scripts/agent_check.sh --probe --asan` on the
// single-iteration probe, 5/5 crashing runs, ASan report `SEGV ... in
// __asan::Allocator::Deallocate`, stack `bz_array_free <-
// bz$patAResultCollFree$OwningTask <- bz_task_free <-
// bz$async$M.C$Run$Resume`); post-fix LSan-clean, correct sum, 5/5 runs.
//
// fix #4670 / §30.5 (v0.307): same conversion as the single-shot row
// above — `using var outer := await C.Wrap(k);` (outer: a Pattern A
// borrow, C.Wrap(k) class-like OwningTask<Cell> result) is rejected
// post-#4670; converted to `var outer`, the scoped await-bind (per-
// iteration hidden slot, disposed at the end of each loop iteration —
// identical disposal timing to the original `using var`).
BEZEL_TEST(PatternATaskResultCollectionMembership3657_3658,
           nested_owning_task_result_amplified_200x_3657) {
    const std::string src =
        "module M; "
        "class Cell "
        "  public value: int; "
        "  public constructor(v: int) value := v; end constructor; "
        "end Cell; "
        "class C "
        "  public static async method Produce(v: int): OwningTask<Cell> "
        "    return new Cell(v); "
        "  end Produce; "
        "  public static async method Wrap(v: int): Task<OwningTask<Cell>> "
        "    return C.Produce(v); "
        "  end Wrap; "
        "  public static async method Run(): Task<int> "
        "    var total := 0; "
        "    for var k in 0..<200 do "
        "      var outer := await C.Wrap(k); "
        "      using var c := await outer; "
        "      total := total + c.value; "
        "    end for; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    using var t := C.Run(); "
        "    t.Wait(); "
        "    Console.WriteLine(t.Result.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Console.WriteLine appends a trailing newline, and (unlike runAotOut)
    // runAotChecked's stdoutText does NOT strip it — assert the byte-exact
    // captured output.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("19900\n"));
}

// ---------------------------------------------------------------------------
// Control row: #3658's kind-dispatch must be reached and behave unchanged
// for every membership-table KIND once membership itself is table-gated.
// `Array`/`Dictionary`/`Stack` are already exercised end-to-end by
// tests/codegen/async_task_intrinsic_collection_result_2279_test.cpp (run
// alongside this file under the same `2279`/`pattern_a` filters, so not
// duplicated here); `LinkedList` has no sibling coverage through THIS
// specific Pattern-A result path, so it is added here as the bz_llist_free
// kind-dispatch control. `OwningBuffer` is deliberately NOT a control row
// here — see the header comment: its own disposal already leaks, pre-
// existing and unrelated to this fix, and is filed as #3684 with its own
// measured repro rather than pinned as an expected failure in this file.
// ---------------------------------------------------------------------------
BEZEL_TEST(PatternATaskResultCollectionMembership3657_3658,
           linked_list_result_control_row_kind_dispatch_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeLinkedList(): Task<LinkedList<int>> "
        "    var ll := new LinkedList<int>(); "
        "    ll.AddLast(10); ll.AddLast(20); ll.AddLast(30); "
        "    return ll; "
        "  end MakeLinkedList; "
        "  public static method Main(): int "
        "    using var t := C.MakeLinkedList(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 3
        "  end Main; "
        "end C;"), 3);
}
