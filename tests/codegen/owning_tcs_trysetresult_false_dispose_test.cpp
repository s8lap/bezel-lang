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
// `SetResult` on an ALREADY-TERMINAL source is the SIBLING site and the same
// root cause. Ruling of record: §30.11's "ownership was transferred at the
// call, and the implementation is responsible for releasing it on every path"
// is a general duty of the `owned` parameter, NOT a remark scoped to the
// `TrySetResult`-returns-`false` branch. `SetResult` throws
// InvalidOperationException on a terminal source, but the operand was still
// handed over at the call, so it is released on that edge too.
//
// SOUNDNESS GATE (this is the part that is easy to get wrong). Disposing the
// rejected operand is correct ONLY when the call expression owns it outright —
// a bare `new T(...)` or an `owned`-returning call. Hand it a NAMED LOCAL and
// that local still carries its own scope-end disposal, so disposing here
// double-frees. MEASURED with the gate absent, under a hand-driven
// `bezelc --sanitize=address` build (172 `__asan` symbols):
//   ==ERROR: AddressSanitizer: heap-use-after-free … READ of size 8
//   freed by thread T0 here: #1 main
// and the SAME program on BASE (arm absent entirely) exits 0 — so the arm
// introduced the fault and the gate removed it. The gate is
// `common::isFreshOwnedClassProducer`, the shared documented classifier for
// exactly this question, so this arm stays on the same definition the rest of
// codegen uses rather than a fourth hand-rolled copy.
//
// ASAN NOTE. The harness ASan leg prints `NATIVE SANITIZER: 0 check(s) skipped
// (ASan/TSan runtime unavailable …)`, which means it cannot be cited as having
// proved anything (#3734). Every ASan number in this file therefore comes from
// a hand-driven `bezelc --sanitize=address` build, whose binary was verified
// to carry 172 `__asan` symbols, with a deliberate 1-`malloc`-4096 leak as the
// sensitivity control proving LSan actually fires on this host.
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

// ===========================================================================
// `SetResult` on an ALREADY-TERMINAL source — the sibling site, same root
// cause, closed by the coordinator's ruling of record.
//
// §30.11 (v0.334), ruling: "ownership was transferred at the call, and the
// implementation is responsible for releasing it ON EVERY PATH" is a general
// duty of the `owned` parameter, not a remark scoped to the
// `TrySetResult`-returns-`false` branch. A terminal source makes `SetResult`
// throw `InvalidOperationException`, but the operand was still handed over at
// the call, so it is released on that edge too.
//
// MEASURED before the fix, under a hand-driven `bezelc --sanitize=address`
// build (172 `__asan` symbols): the counter read 2 inside the source's scope
// where it reads 1 with the fix, and LSan reported the 16-byte instance:
//
//   threw
//   2          <- the thrown operand, which nothing reclaimed
//   1
//   ==ERROR: LeakSanitizer: detected memory leaks
//   Direct leak of 16 byte(s) ... SUMMARY: 16 byte(s) leaked
//
// The DURING-scope count is the whole assertion. The final count is 1 on base
// and with the fix alike (the source's disposal releases the task's own result
// either way), so asserting only the final value would pin nothing.
// ===========================================================================
BEZEL_TEST(owning_tcs_trysetresult_false_dispose,
           setresult_terminal_throw_releases_handed_result_30_11) {
    auto r = runAotChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static method Main(): int "
        "    var threw := false; "
        "    do "
        "      using var tcs := new OwningTaskCompletionSource<Cell>(); "
        // First: ACCEPTED. The task owns that Cell until the source's disposal.
        "      tcs.SetResult(new Cell()); "
        // Second: the task is terminal, so this THROWS. The Cell handed to the
        // rejected call must still be released on that edge.
        "      try "
        "        tcs.SetResult(new Cell()); "
        "      catch e: InvalidOperationException => "
        "        threw := true; "
        "      end try; "
        "      if not threw then return -1; end if; "
        // THE SHARP ASSERTION: only the task's own Cell exists during the
        // source's scope. Pre-fix this reads 2.
        "      if Cell.n <> 1 then return -2; end if; "
        "    end do; "
        "    return Cell.n; "                               // expect 0
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// DIFFERENTIAL CONTROL for the row above — the ACCEPTED `SetResult` must still
// hand ownership to the task, and only to the task. Same program, one
// `SetResult`, no throwing edge: the counter reads 1 inside the source's scope
// (the task owns it) and 0 after (the source's disposal released it). If the
// arm over-fired on the accepted path, this would read 0 inside the scope.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_tcs_trysetresult_false_dispose,
           accepted_setresult_leaves_result_with_the_task_30_11) {
    auto r = runAotChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static method Main(): int "
        "    var inScope := 0; "
        "    do "
        "      using var tcs := new OwningTaskCompletionSource<Cell>(); "
        "      tcs.SetResult(new Cell()); "
        "      inScope := Cell.n; "
        "    end do; "
        "    if inScope <> 1 then return -1; end if; "   // owned by the task only
        "    return Cell.n; "                            // expect 0
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// THE SOUNDNESS GATE — the operand owner decides, not the call.
//
// The false-path disposal is correct ONLY for an operand the call EXPRESSION
// owns outright (a bare `new T(...)`, or an `owned`-returning call). Hand it a
// NAMED LOCAL instead and that local keeps its own scope-end disposal, so a
// disposal here double-frees.
//
// MEASURED with the gate absent, under a hand-driven
// `bezelc --sanitize=address` build:
//   ==ERROR: AddressSanitizer: heap-use-after-free on address 0x…
//   READ of size 8 … freed by thread T0 here: #1 main
// with the gate present: exit 0, no sanitizer report, and the SAME program on
// BASE (the arm absent entirely) is also exit 0 — so the use-after-free was
// introduced by the arm and removed by the gate, not merely shifted.
//
// `runAotChecked` drives the LSan net; the use-after-free is caught by the
// separate ASan leg below.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_tcs_trysetresult_false_dispose,
           rejected_call_does_not_double_free_a_named_local_operand_30_11) {
    auto r = runAotChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var tcs := new OwningTaskCompletionSource<Cell>(); "
        "      tcs.TrySetResult(new Cell()); "           // accepted: terminal
        "      var held := new Cell(); "                  // NAMED local operand
        "      if tcs.TrySetResult(held) then return -1; end if; "  // rejected
        "      held.Dispose(); "                         // explicit, not scope-end
        "    end do; "
        "    return Cell.n; "                             // expect 0
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// The gate must not have narrowed the fix away: the TEMPORARY form — the one
// that actually leaked — still disposes on the rejected edge. Same program as
// `trysetresult_false_path_disposes_handed_result_30_11` but read during the
// source's scope, which is where the leak was visible.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_tcs_trysetresult_false_dispose,
           temporary_operand_still_disposed_on_rejected_edge_30_11) {
    auto r = runAotChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var tcs := new OwningTaskCompletionSource<Cell>(); "
        "      if not tcs.TrySetResult(new Cell()) then return -1; end if; "
        // A bare `new Cell()` TEMPORARY: no other owner, so the rejected call
        // must release it. Pre-fix this leaves the count at 2 here.
        "      if tcs.TrySetResult(new Cell()) then return -2; end if; "
        "      if Cell.n <> 1 then return -3; end if; "
        "    end do; "
        "    return Cell.n; "                             // expect 0
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}