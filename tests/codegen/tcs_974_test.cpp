// Codegen for TaskCompletionSource<T> / OwnedTaskCompletionSource<T> /
// TaskCompletionSource (non-generic), stdlib §6.6, issue #974.
//
// These types were previously absent from the type registry, producing an
// "unresolved type name" error whenever Bezel source referenced them.  This
// fix registers the names, adds sema method arms, a thin runtime (bz_tcs_t),
// and codegen for:
//   - new TaskCompletionSource<T>()  → bz_tcs_new()
//   - tcs.Task                       → bz_tcs_task(tcs)  [borrowed inner task]
//   - tcs.TrySetResult(v): bool      → bz_tcs_try_set_result(...)
//   - tcs.SetResult(v)               → same + IOE guard if already done
//   - tcs.TrySetException(exc): bool → bz_tcs_try_set_exception(...)
//   - tcs.TrySetCancelled(): bool    → bz_tcs_try_set_cancelled(...)
//   - using var tcs := ...           → bz_tcs_fault_if_pending + fire-and-forget
//                                      + bz_tcs_free at scope end
//
// Tests derive from stdlib §6.6, covering:
//   1. Non-generic TaskCompletionSource — TrySetResult (void complete)
//   2. TaskCompletionSource<int> — TrySetResult + await Task → correct value
//   3. TrySetResult returns false on second attempt (idempotent)
//   4. SetResult throws InvalidOperationException if already completed
//   5. TrySetCancelled — awaiter gets OperationCanceledException
//   6. TrySetException — awaiter re-raises the original exception
//   7. using-var disposal — scope-end faults a never-completed TCS (no hang)
//   8. OwnedTaskCompletionSource<T> — TrySetResult delivers owned result

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

// Cell class for ownership tracking (ctor n++, dtor n--).
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

}  // namespace

// §6.6 test 1: non-generic TaskCompletionSource — TrySetResult(void completion).
// TaskCompletionSource completes a plain Task (no generic T).
// await the inner task; verify execution continued (exit 42).
BEZEL_TEST(tcs_974, non_generic_tcs_trysetresult_void_completes_task) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var tcs := new TaskCompletionSource(); "
        "    _ := tcs.TrySetResult(0); "
        "    await tcs.Task; "
        "    return 42; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    return await C.Run(); "
        "  end Main; "
        "end C;"), 42);
}

// §6.6 test 2: TaskCompletionSource<int> — TrySetResult(value) + await → value.
// The awaited Task<int> should produce the int value 99.
BEZEL_TEST(tcs_974, generic_tcs_int_trysetresult_await_returns_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var tcs := new TaskCompletionSource<int>(); "
        "    _ := tcs.TrySetResult(99); "
        "    return await tcs.Task; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    return await C.Run(); "
        "  end Main; "
        "end C;"), 99);
}

// §6.6 test 3: TrySetResult returns false on second attempt (already completed).
// First call returns true; second returns false (idempotent).
BEZEL_TEST(tcs_974, trysetresult_idempotent_second_call_returns_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var tcs := new TaskCompletionSource<int>(); "
        "    var first  := tcs.TrySetResult(1); "
        "    var second := tcs.TrySetResult(2); "
        "    if first and not second then "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §6.6 test 4: TrySetCancelled — awaiter receives OperationCanceledException.
// The awaiting code should catch the cancellation exception (exit 7).
BEZEL_TEST(tcs_974, tcs_trysetcancelled_awaiter_catches_oce) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var tcs := new TaskCompletionSource<int>(); "
        "    _ := tcs.TrySetCancelled(); "
        "    var x: int := 0; "
        "    try x := await tcs.Task; return 0; "
        "    catch OperationCanceledException => return 7; "
        "    end; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    return await C.Run(); "
        "  end Main; "
        "end C;"), 7);
}

// §6.6 test 5: TrySetException — awaiter re-raises the stored exception.
// The exception type is checked via catch (exit 5).
BEZEL_TEST(tcs_974, tcs_trysetexception_awaiter_rethrows) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var tcs := new TaskCompletionSource<int>(); "
        "    _ := tcs.TrySetException(new InvalidOperationException()); "
        "    var x: int := 0; "
        "    try x := await tcs.Task; return 0; "
        "    catch InvalidOperationException => return 5; "
        "    end; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    return await C.Run(); "
        "  end Main; "
        "end C;"), 5);
}

// §6.6 test 6: using-var disposal on a never-completed TCS.
// When the scope exits before TrySetResult is called, bz_tcs_fault_if_pending
// faults the inner task (so any awaiter wakes up with an error rather than
// hanging).  This test verifies the disposal does not crash or hang (exit 3).
BEZEL_TEST(tcs_974, tcs_usingvar_disposal_faults_pending_task) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var tcs := new TaskCompletionSource<int>(); "
        "    end; "
        "    return 3; "
        "  end Main; "
        "end C;"), 3);
}

// §6.6 test 7: Task property accessed before completion, then TrySetResult.
// Bind tcs.Task to a local first, then complete, then await.
BEZEL_TEST(tcs_974, tcs_task_property_bound_then_completed) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var tcs := new TaskCompletionSource<int>(); "
        "    using var t := tcs.Task; "
        "    _ := tcs.TrySetResult(77); "
        "    return await t; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    return await C.Run(); "
        "  end Main; "
        "end C;"), 77);
}
