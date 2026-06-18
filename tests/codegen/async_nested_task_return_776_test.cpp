// fix #776 / §30.5: returning a bare local `Task<T>` as the result of a
// `Task<Task<T>>` async method no longer hangs.
//
// `Outer(): Task<Task<int>> var t := Inner(); return t;` returns the inner task
// `t` as Outer's result. The async return lowering ran emitAllDisposals BEFORE
// emitAsyncComplete, so scope-end disposal fire-and-forgot the very task being
// returned — the caller's `await` on it then hung (its completion continuation
// had been consumed by the fire-and-forget). The fix nulls the returned
// Task-typed local's slot before scope-end disposal (the task pointer is already
// loaded into the result value), so the redundant scope-end dispose is skipped.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §30.5: the #776 repro — return a bare inner task. Was an infinite hang; now
// completes and the doubly-awaited value flows through (7).
BEZEL_TEST(async_nested_task_return_776, return_inner_task_completes) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static async method Inner(): Task<int> return 7; end Inner; "
        "  public static async method Outer(): Task<Task<int>> "
        "    var t := Inner(); "
        "    return t; "  // transfer the inner task as Outer's result
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    var tt := await Outer(); "
        "    return await tt; "  // 7
        "  end Main; "
        "end C;"), 7);
}

// §30.5: the inner task's computed value flows through correctly (not just
// "doesn't hang") — Inner(4) → 40.
BEZEL_TEST(async_nested_task_return_776, inner_task_value_flows) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static async method Inner(n: int): Task<int> "
        "    return n * 10; end Inner; "
        "  public static async method Outer(): Task<Task<int>> "
        "    var t := Inner(4); "
        "    return t; "
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    var tt := await Outer(); "
        "    return await tt; "  // 40
        "  end Main; "
        "end C;"), 40);
}

// §30.5 regression guard: an ordinary `return await t` (NOT a bare task-local
// return) is unaffected by the fix and still completes.
BEZEL_TEST(async_nested_task_return_776, return_await_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static async method Inner(): Task<int> return 5; end Inner; "
        "  public static async method Outer(): Task<int> "
        "    var t := Inner(); "
        "    return await t; "  // awaited, not transferred
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    return await Outer(); "  // 5
        "  end Main; "
        "end C;"), 5);
}
