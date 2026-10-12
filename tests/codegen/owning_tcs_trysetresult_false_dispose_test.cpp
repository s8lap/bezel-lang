// Codegen — §30.11 / stdlib §6.6 `OwningTaskCompletionSource<T>.TrySetResult`
// false path must dispose the handed-over result.
//
// Spec §30.11, verbatim:
//   "If `TrySetResult` returns `false` because the task is already in a
//    terminal state, the implementation disposes `result` before returning.
//    The caller has no further obligation — ownership was transferred at the
//    call, and the implementation is responsible for releasing it on every
//    path."
// and, symmetrically:
//   "The same applies if `TrySetException` or `TrySetCancelled` returns
//    `false` while a previous `SetResult` already took ownership of a result;
//    the existing result remains owned by the task."
//
// Pre-fix (src/codegen/LlvmBackend_Calls3.cpp, Emitter::tryLowerTcsCall, the
// `TrySetResult` / `SetResult` arm), the value was lowered into a stack slot,
// handed to `bz_tcs_try_set_result`, and then DROPPED on the false path: the
// arm returned the i1 and nothing else. `bz_tcs_try_set_result` returns 0
// without copying (`compare_exchange_strong` on `tcs->completed` fails first),
// so nothing downstream reclaimed it either. The `owned` parameter's
// discharge obligation — which sema had already discharged at the call, §30.11
// "ownership was transferred at the call" — had no lowering, so every rejected
// `TrySetResult(owned r)` leaked one class instance, for the life of the
// process.
//
// Oracle: `Cell`-counter idiom (ctor ++, dtor --), looped 100x so the
// dispose-exactly-once claim is not satisfied by luck on one iteration.
// `Cell.n == 0` after the source's scope is the assertion.
//
// The suite also carries:
//   - a DIFFERENTIAL CONTROL: the FIRST (accepted) `TrySetResult` must still
//     hand the result to the task, which owns it until the task is disposed.
//     If the fix over-fired, `Cell.n` would read 0 while the source is still
//     live — the control catches that.
//   - a class-TYPE read on `TaskCompletionSource<T>`'s twin arm: the
//     non-owning source's `TrySetResult(result: T)` takes a PLAIN value
//     (§30.11's intrinsic restriction — its T is a non-class type), so it must
//     keep compiling and must NOT grow a disposal.
//
// Fail-without: reverting src/codegen/LlvmBackend_Calls3.cpp to the base SHA
// makes `trysetresult_false_path_disposes_handed_result_30_11` print 2 where
// it prints 0 (and LSan report the leak under runAotChecked).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Cell: static counter (ctor ++, dtor --). A leak leaves Cell.n > 0; a
// double-dispose underflows it or crashes. Same idiom as
// async_task_result_disposal_1503_test.cpp's kCell.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// The loop count. §30.11's rule is about EVERY rejected TrySetResult, so one
// iteration would be a vacuous oracle; 100 rejections of a fresh Cell each is
// enough that a one-shot leak cannot hide.
constexpr int kIters = 100;

}  // namespace

// ---------------------------------------------------------------------------
// §30.11: a REJECTED `TrySetResult(owned result: T)` disposes the result it
// was handed. Looped kIters times. The in-loop `Cell.n <> 1` read is the sharp
// part: within ONE iteration the accepted hand-over has put the task's Cell in
// existence and the rejected one must be gone before the loop body ends, so
// the counter can never exceed 1 at any point. A leak that only drained at
// scope exit would read 2 there.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_tcs_trysetresult_false_dispose,
           trysetresult_false_path_disposes_handed_result_30_11) {
    auto r = runAotChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < " + std::to_string(kIters) + " do "
        "      using var tcs := new OwningTaskCompletionSource<Cell>(); "
        // First hand-over: ACCEPTED. The task now owns that Cell, and holds
        // it until the task is disposed (§30.11 *Common semantics*).
        "      if not tcs.TrySetResult(new Cell()) then return -1; end if; "
        // Second hand-over: REJECTED — the task is already in a terminal
        // state. §30.11 requires the implementation to dispose THIS Cell.
        "      if tcs.TrySetResult(new Cell()) then return -2; end if; "
        // expect exactly 1: the task's Cell only.
        "      if Cell.n <> 1 then return -3; end if; "
        "      i := i + 1; "
        "    end while; "
        // Every source was disposed at its scope end, releasing the result it
        // owned. expect 0.
        "    return Cell.n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// DIFFERENTIAL CONTROL — the ACCEPTED `TrySetResult` must be untouched. The
// task owns the result it was handed; it is released by the SOURCE's disposal
// (§30.11: "For either kind, a never-awaited task is disposed exactly once, by
// the source at source disposal"), NOT by the accepted call. So `Cell.n` reads
// 1 inside the source's scope and 0 after it.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_tcs_trysetresult_false_dispose,
           accepted_trysetresult_leaves_result_with_the_task_30_11) {
    auto r = runAotChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static method Main(): int "
        "    using var tcs := new OwningTaskCompletionSource<Cell>(); "
        "    if not tcs.TrySetResult(new Cell()) then return -1; end if; "
        "    if Cell.n <> 1 then return -2; end if; "   // still owned by tcs
        "    return Cell.n; "                            // expect 1
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ---------------------------------------------------------------------------
// DIFFERENTIAL CONTROL (disposal lands) — the SAME source, but the `Cell.n`
// read moved OUTSIDE the `using var tcs` scope. The task is disposed at scope
// end and releases the result IT owned, so the counter returns to 0. If the
// new false-path disposal had also fired on the ACCEPTED hand-over, this would
// read -1 (a double dispose) or crash.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_tcs_trysetresult_false_dispose,
           source_disposal_releases_the_accepted_result_30_11) {
    auto r = runAotChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var tcs := new OwningTaskCompletionSource<Cell>(); "
        "      if not tcs.TrySetResult(new Cell()) then return -1; end if; "
        "    end do; "
        "    return Cell.n; "                            // expect 0
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// DIFFERENTIAL CONTROL (the non-owning twin) — §30.11's intrinsic restriction
// admits `TaskCompletionSource<T>` only at a NON-CLASS T, whose `TrySetResult
// (result: T)` takes a plain value with no disposal obligation. The added
// false-path arm is gated on the OwningTaskCompletionSource receiver, so this
// must keep compiling and keep running unchanged.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_tcs_trysetresult_false_dispose,
           non_owning_twin_keeps_plain_value_semantics_30_11) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    if not tcs.TrySetResult(1) then return -1; end if; "
        "    if tcs.TrySetResult(2) then return -2; end if; "
        "    if tcs.Task.Result <> 1 then return -3; end if; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// THE REPRO AS THE ISSUE SHAPED IT — the §30.11 worked pattern with a
// `SetResult` first and a rejected `TrySetResult` second, asserting the
// rejected operand is gone while the task's own result survives, and that the
// source's disposal clears both. Program form only; the counter assertions
// above carry the dispose-exactly-once claim.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_tcs_trysetresult_false_dispose,
           rejected_operand_released_accepted_result_kept_30_11) {
    auto r = runAotChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var tcs := new OwningTaskCompletionSource<Cell>(); "
        "      tcs.SetResult(new Cell()); "                 // accepted: task owns it
        "      if tcs.TrySetResult(new Cell()) then return -1; end if; "  // rejected
        "      if Cell.n <> 1 then return -2; end if; "     // only the task's remains
        "    end do; "
        "    return Cell.n; "                               // expect 0
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}