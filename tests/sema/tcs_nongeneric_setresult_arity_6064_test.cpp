// Spec-derived regression test for issue #6064.
//
// §30.11 (v0.338) / stdlib §6.6: the NON-GENERIC `TaskCompletionSource` (for a
// `Task` with no result) has the same shape as `TaskCompletionSource<T>` minus
// the result parameter on SetResult/TrySetResult:
//
//     public method SetResult();
//     public method TrySetResult(): bool;
//
// The sema arm for the TaskCompletionSource family (ExprTypecheck_Calls.cpp,
// `rcn == "TaskCompletionSource"`) types these calls and never checks the
// argument list, so `tcs.SetResult(1)`, `tcs.SetResult(1, 2)` and
// `tcs.TrySetResult(1)` on the non-generic receiver were silently accepted.
// Calls with arguments must be rejected at sema with the arity diagnostic.
//
// The rows here are runAot oracles: reject rows assert the located diagnostic
// text, accept rows run the program and assert stdout. Generic
// `TaskCompletionSource<T>` calls are unchanged and keep their accept rows.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// REJECT — the issue's own repro: one argument on the non-generic SetResult.
// The located diagnostic sits at the callee's span (line 5, column 5).
BEZEL_TEST(sema_tcs_nongeneric_setresult_arity_6064,
           nongeneric_setresult_one_arg_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var tcs := new TaskCompletionSource();\n"
        "    tcs.SetResult(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:5: `SetResult` has no overload that "
                                      "accepts 1 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT — two arguments on the non-generic SetResult (issue repro `SetResult(1, 2)`).
BEZEL_TEST(sema_tcs_nongeneric_setresult_arity_6064,
           nongeneric_setresult_two_args_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var tcs := new TaskCompletionSource();\n"
        "    tcs.SetResult(1, 2);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:5: `SetResult` has no overload that "
                                      "accepts 2 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// REJECT — TrySetResult(1) on the non-generic receiver (issue repro).
BEZEL_TEST(sema_tcs_nongeneric_setresult_arity_6064,
           nongeneric_trysetresult_one_arg_rejected_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var tcs := new TaskCompletionSource();\n"
        "    var ok: bool := tcs.TrySetResult(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":5:21: `TrySetResult` has no overload "
                                      "that accepts 1 argument(s)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§30.11"));
}

// ACCEPT (control) — the spec-form zero-argument TrySetResult(): bool on the
// non-generic receiver must stay accepted by sema. The assertion is that the
// program is accepted and runs to its print; the completion's observable
// effect is not asserted here (it is pinned by the codegen lowering rows).
BEZEL_TEST(sema_tcs_nongeneric_setresult_arity_6064,
           nongeneric_trysetresult_zero_args_accepted_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var tcs := new TaskCompletionSource();\n"
        "    var ok: bool := tcs.TrySetResult();\n"
        "    Console.WriteLine(\"done\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// ACCEPT (control) — the GENERIC TaskCompletionSource<int> keeps its one-argument
// SetResult(result: T); the non-generic arity rule must not reach it.
BEZEL_TEST(sema_tcs_nongeneric_setresult_arity_6064,
           generic_setresult_one_arg_still_accepted_spec_30_11) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public static async method Run(): Task<int>\n"
        "    using var tcs := new TaskCompletionSource<int>();\n"
        "    tcs.SetResult(5);\n"
        "    return await tcs.Task;\n"
        "  end Run;\n"
        "  public static async method Main(): Task<int>\n"
        "    Console.WriteLine(await C.Run());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}
