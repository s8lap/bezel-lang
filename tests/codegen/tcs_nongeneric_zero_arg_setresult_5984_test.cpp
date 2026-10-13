// Codegen — §30.11 (stdlib §6.6, v0.338) non-generic `TaskCompletionSource`:
// the zero-argument `SetResult()` / `TrySetResult()` must complete the task.
//
// Spec §30.11, v0.338, verbatim: "A non-generic `TaskCompletionSource` (for
// `Task` with no result) has the same shape minus the `result` parameter on
// `SetResult`/`TrySetResult`."
//
// Pre-fix (src/codegen/LlvmBackend_Calls3.cpp, Emitter::tryLowerTcsCall), the
// `TrySetResult` / `SetResult` arm only ran under `!call.args.empty()`. A
// zero-argument call fell through to `return nullptr` with no emitted code:
// `SetResult()` on a static field compiled silently and never completed the
// task (an `await` hung), and `TrySetResult()` reported `false` without
// completing anything. The local-receiver form was rejected by the #2457
// "no dispatch arm emitted any code" backstop instead.
//
// Rows (all runAot; the observable is captured stdout, not the exit code):
//   - static field, instance field, local: `SetResult()` completes the task
//   - static field: `await` on the task resumes (the #5984 hang)
//   - `TrySetResult()` returns `true` on the first call and `false` after
//   - control: generic `TaskCompletionSource<int>.SetResult(7)` still completes

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// §30.11 v0.338: static-field receiver. Pre-fix: prints `False` (the call was
// a silent no-op).
BEZEL_TEST(codegen_tcs_nongeneric_zero_arg_setresult_5984,
           static_field_setresult_completes_task_30_11_v0338) {
    auto r = runAot(
        "module Conf; "
        "public class Demo "
        "  public static owned readonly Done: TaskCompletionSource := new TaskCompletionSource(); "
        "  public static method Main() "
        "    Console.WriteLine(\"before\"); "
        "    Demo.Done.SetResult(); "
        "    Console.WriteLine(\"after\"); "
        "    Console.WriteLine(Demo.Done.Task.IsCompleted.ToString()); "
        "  end Main; "
        "end Demo;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "before\nafter\nTrue\n"));
    BEZEL_EXPECT_FALSE(contains(r.stdoutText, "False"));
}

// §30.11 v0.338 + §6.6: an `await` on the task completed by a zero-argument
// `SetResult()` must resume. Pre-fix: the await never returns (run timeout).
BEZEL_TEST(codegen_tcs_nongeneric_zero_arg_setresult_5984,
           static_field_await_resumes_after_setresult_30_11_v0338) {
    auto r = runAot(
        "module Conf; "
        "public class Demo "
        "  public static owned readonly Done: TaskCompletionSource := new TaskCompletionSource(); "
        "  public static async method Main(): Task "
        "    Demo.Done.SetResult(); "
        "    await Demo.Done.Task; "
        "    Console.WriteLine(\"done\"); "
        "  end Main; "
        "end Demo;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "done\n"));
}

// §30.11 v0.338: instance field receiver inside an instance method.
BEZEL_TEST(codegen_tcs_nongeneric_zero_arg_setresult_5984,
           instance_field_setresult_completes_task_30_11_v0338) {
    auto r = runAot(
        "module Conf; "
        "public class Holder "
        "  public owned readonly Done: TaskCompletionSource := new TaskCompletionSource(); "
        "  public method Fire() "
        "    Done.SetResult(); "
        "  end Fire; "
        "  public method Ready(): bool "
        "    return Done.Task.IsCompleted; "
        "  end Ready; "
        "end Holder; "
        "public class M "
        "  public static method Main() "
        "    using var h := new Holder(); "
        "    Console.WriteLine(h.Ready().ToString()); "
        "    h.Fire(); "
        "    Console.WriteLine(h.Ready().ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "False\nTrue\n"));
}

// §30.11 v0.338, local receiver. Pre-fix: rejected by the #2457 backstop.
BEZEL_TEST(codegen_tcs_nongeneric_zero_arg_setresult_5984,
           local_setresult_completes_task_30_11_v0338) {
    auto r = runAot(
        "module Conf; "
        "public class M "
        "  public static method Main() "
        "    using var tcs := new TaskCompletionSource(); "
        "    tcs.SetResult(); "
        "    Console.WriteLine(tcs.Task.IsCompleted.ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "True\n"));
}

// §30.11 v0.338: `TrySetResult()` (zero-argument, non-generic) returns `true`
// when it completes the task and `false` once the task is terminal. Pre-fix:
// both calls return `false` and the task is never completed.
BEZEL_TEST(codegen_tcs_nongeneric_zero_arg_setresult_5984,
           trysetresult_zero_arg_reports_first_win_30_11_v0338) {
    auto r = runAot(
        "module Conf; "
        "public class M "
        "  public static method Main() "
        "    using var tcs := new TaskCompletionSource(); "
        "    var first := tcs.TrySetResult(); "
        "    var second := tcs.TrySetResult(); "
        "    Console.WriteLine(first.ToString()); "
        "    Console.WriteLine(second.ToString()); "
        "    Console.WriteLine(tcs.Task.IsCompleted.ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "True\nFalse\nTrue\n"));
}

// Control: the generic form with its `result` argument is unchanged by the fix.
BEZEL_TEST(codegen_tcs_nongeneric_zero_arg_setresult_5984,
           generic_setresult_with_argument_control_30_11_v0338) {
    auto r = runAot(
        "module Conf; "
        "public class M "
        "  public static method Main() "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    tcs.SetResult(7); "
        "    Console.WriteLine(tcs.Task.IsCompleted.ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(contains(r.stdoutText, "True\n"));
}
