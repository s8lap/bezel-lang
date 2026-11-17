// Ownership — fix #8671: the #8616 completion-source Task escape check did
// not look through the postfix `!`, so `return tcs.Task!;` and
// `return tcs?.Task!;` still escaped the source's scope (the task hangs when
// read).
//
// Spec (revision v0.357): §30.11 *Task-object ownership* ("the task never
// outlives its source"; returning `tcs.Task` itself out of the scope is
// rejected by §28.3's borrowed-return rule); §28.3 *Borrow lifetime rules*.
// The user-class analog `return h.It!;` is already rejected.
//
// Every row is RED without the fix and green with it.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
constexpr const char* kReturnDiag =
    "borrow returned from `tcs` would outlive its source";
constexpr const char* kFieldDiag = "stored into a field would outlive its source";

std::string prog(const char* retExpr) {
    return std::string(
               "module M; "
               "public class C "
               "  public static method Get(): Task<int> "
               "    using var tcs := new TaskCompletionSource<int>(); "
               "    tcs.SetResult(1); "
               "    return ") + retExpr + "; "
           "  end Get; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}
}  // namespace

BEZEL_TEST(tcs_task_borrow_escape_8671, reject_return_dot_task_bang_30_11) {
    auto r = runAot(prog("tcs.Task!"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReturnDiag));
}

BEZEL_TEST(tcs_task_borrow_escape_8671, reject_return_dot_task_double_bang_30_11) {
    auto r = runAot(prog("tcs.Task!!"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReturnDiag));
}

BEZEL_TEST(tcs_task_borrow_escape_8671, reject_return_nullcond_task_bang_30_11) {
    auto r = runAot(prog("tcs?.Task!"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReturnDiag));
}

BEZEL_TEST(tcs_task_borrow_escape_8671, reject_field_store_task_bang_30_11) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  private _t: Task<int>?; "
        "  public method Fill(): int "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    tcs.SetResult(1); "
        "    _t := tcs.Task!; "
        "    return 0; "
        "  end Fill; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFieldDiag));
}
