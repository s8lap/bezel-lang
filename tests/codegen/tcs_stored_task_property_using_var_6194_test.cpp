// Codegen — issue #6194 (crash): the `using var` / `await using var` of a
// STORED-TASK SOURCE's `Task` property HANGS under AOT.
//
// Spec basis (v0.345):
//   §30.11 (TaskCompletionSource<T> / OwningTaskCompletionSource<T>,
//   "Task-object ownership"): "The **source owns the task object** its `Task`
//   property exposes. A read of the property is a **borrow** of the task,
//   sourced at the source object's location — the same receiver-sourced
//   notion as an owned-field read (§28.6) — ... there is no first-read
//   ownership transfer." And: "For either kind, a **never-awaited** task is
//   disposed exactly once, by the source at source disposal."
//   §28.10 ("`using var` initialized from a borrow"): over a borrow the
//   binding "does **not** assume a disposal obligation ... at scope end, no
//   `Dispose()` is called, because the value is not owned by this scope."
//   stdlib §6.6: "Stored tasks are exposed as **properties**
//   (`TaskCompletionSource<T>.Task`, `OwningTaskCompletionSource<T>.Task`),
//   whose reads are borrows of the source's storage."
//
// THE DEFECT. `using var t := tcs.Task;` registered a `d.task` scope-end
// disposal, so at scope end the inner task was fire-and-forgotten and FREED.
// The source's own scope-end release then read the SAME inner task again
// (`bz_tcs_task` -> `bz_task_is_error`) — a use-after-free. The first lock on
// the freed task's `std::mutex` blocks forever, so the program HANGS (rc=124
// at a 20 s limit) instead of faulting; under ASan the same shape is a SEGV
// when the task is still pending.
//
// MEASURED on this tree's base (c5e8992d), AOT, ASan, 20 s limit:
//   using var tcs := new TaskCompletionSource<int>();
//   using var t  := tcs.Task;      -> rc=124 (HANG)
//   await using var t := tcs.Task; -> rc=124 (HANG)
//   var      t := tcs.Task;        -> rc=77  (the plain-`var` twin: correct)
//   return await tcs.Task;         -> rc=77  (the canonical §30.11 spelling)
//
// The plain-`var` twin is correct because the ownership classification for a
// storage READ is a borrow (`taskLocalInitializerIsBorrowSource` reads a
// member access as a read, not a mint) — the `using var` spelling did not ask
// that question, and the ownership pass's own borrow-pin does not cover a
// builtin property read.
//
// THE ORACLE is the HARNESS verdict, not the exit code: a sanitizer report, a
// crash, or a run that never terminates all surface as ran=false and collapse
// to the -999 sentinel. Pre-fix the shape is a HANG (a timeout, i.e. a
// 30 s-by-default run deadline), post-fix it completes and exits 77.
//
// The program is VALID and must exit 77: §30.11 makes the property read a
// borrow of the source's storage, §28.10 admits a `using var` over a borrow
// (as a pin that disposes nothing), and the source's disposal is the task's
// single disposal.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The oracle is the HARNESS verdict: a sanitizer report, a crash, or a run
// that never terminates all surface as ran=false -> -999.
int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// #6194 — the traced shape: a `using var` over the stored task.
// Fails without the fix: the scope-end task disposal freed the source's inner
// task; the source's own release then locked the freed task's mutex and the
// process hung forever -> runAotChecked timeout -> -999 != 77.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_using_var_6194,
           using_var_over_stored_task_property_is_a_borrow_pin_spec_30_11) {
    // per §30.11 / §28.10 (v0.345)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    using var t := tcs.Task; "
        "    _ := tcs.TrySetResult(77); "
        "    return await t; "
        "  end Main; "
        "end C;"), 77);
}

// ---------------------------------------------------------------------------
// #6194 — the AWAITED spelling of the same shape. §28.12's "await using" takes
// §28.10's admission unchanged, and over a BORROW right-hand side "the
// binding assumes no obligation, so scope exit disposes nothing and awaits
// nothing" — so the awaited spelling must behave exactly like the plain one.
//
// Fails without the fix: the same double free and the same hang.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_using_var_6194,
           await_using_var_over_stored_task_property_is_a_borrow_pin_spec_30_11) {
    // per §30.11 / §28.12 / §28.10 (v0.345)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    await using var t := tcs.Task; "
        "    _ := tcs.TrySetResult(77); "
        "    return await t; "
        "  end Main; "
        "end C;"), 77);
}

// ---------------------------------------------------------------------------
// #6194 — CONTROL 1: the plain-`var` twin. It was already correct (a storage
// READ is a borrow) and must STAY correct: the fix must not change it. This
// row is the twin that distinguishes "the fix repaired the `using var`
// spelling" from "the fix made the `using var` spelling agree with a
// behaviour that was itself wrong".
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_using_var_6194,
           plain_var_over_stored_task_property_still_exits_77_spec_30_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    var t := tcs.Task; "
        "    _ := tcs.TrySetResult(77); "
        "    return await t; "
        "  end Main; "
        "end C;"), 77);
}

// ---------------------------------------------------------------------------
// #6194 — CONTROL 2: the CANONICAL §30.11 spelling, `return await tcs.Task;`.
// §30.11's own worked example. It was already correct and must stay correct:
// the fix pins a BORROW-PIN on the BOUND spelling, and must not disturb the
// unbound one.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_using_var_6194,
           canonical_await_tcs_task_spelling_still_exits_77_spec_30_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    _ := tcs.TrySetResult(77); "
        "    return await tcs.Task; "
        "  end Main; "
        "end C;"), 77);
}

// ---------------------------------------------------------------------------
// #6194 — the OwningTaskCompletionSource<T> twin: the same stored-task
// property, the other source class. §30.11's rule is stated for "either
// source", so the borrow-pin must hold for both.
//
// Fails without the fix: the same double free and the same hang.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_using_var_6194,
           owning_tcs_stored_task_property_is_a_borrow_pin_spec_30_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Res "
        "  public constructor() end constructor; "
        "end Res; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tcs := new OwningTaskCompletionSource<Res>(); "
        "    using var t := tcs.Task; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}
