// Ownership — fix #8616: returning a completion source's `Task` property out
// of the scope that owns the source was accepted, in BOTH spellings (`tcs.Task`
// and `tcs?.Task`), and the program hung when the returned task was read.
//
// Spec (revision v0.357):
//   §30.11 *Task-object ownership*: "The **source owns the task object** its
//   `Task` property exposes. A read of the property is a borrow of the task,
//   sourced at the source object's location ... for the canonical `using var
//   tcs` shape, the borrow is bounded by `tcs`'s scope, so `return await
//   tcs.Task;` inside the scope ... is well-formed, while returning `tcs.Task`
//   *itself* out of the scope is the return of a borrow sourced at a local with
//   no source that outlives the call — rejected by §28.3's borrowed-return
//   rule ... The task never outlives its source."
//   §28.3 *Borrow lifetime rules*: a borrow whose source is a scope-bound owned
//   local may not be returned, nor stored into a field.
//
// ROOT CAUSE. `Task<T>` / `OwningTask<T>` / `Task` are declaration-less
// builtin heads, which `isBorrowableType` reads as non-borrowable, so
// `returnedExprMayBorrow` answered false for `tcs.Task` and the whole
// §28.3 return funnel (`checkBorrowEscapesOnReturn`) was skipped. The `?.`
// spelling was rejected only by accident until #7423 typed it.
//
// Every row below is RED without the fix (compiles clean) and green with it.
// The must-stay-legal shapes (await inside the scope, a call-bounded argument,
// a field or parameter source) are corpus candidates, not rows here.

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
}  // namespace

// The issue's `?.` repro, verbatim modulo the Main body.
BEZEL_TEST(tcs_task_borrow_escape_8616, reject_return_nullcond_task_30_11) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public static method Get(): Task<int>? "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    _ := tcs.TrySetResult(5); "
        "    return tcs?.Task; "
        "  end Get; "
        "  public static method Main(): int "
        "    var t := Get(); "
        "    var d: bool? := t?.IsCompleted; "
        "    Console.WriteLine(d ?? false); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReturnDiag));
}

// The `.` spelling (the issue's `p9`).
BEZEL_TEST(tcs_task_borrow_escape_8616, reject_return_dot_task_30_11) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public static method Get(): Task<int>? "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    _ := tcs.TrySetResult(5); "
        "    return tcs.Task; "
        "  end Get; "
        "  public static method Main(): int "
        "    var t := Get(); "
        "    var d: bool? := t?.IsCompleted; "
        "    Console.WriteLine(d ?? false); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReturnDiag));
}

// Corpus case `s30_11_0003`'s program: the non-nullable `Task<int>` return.
BEZEL_TEST(tcs_task_borrow_escape_8616, reject_return_dot_task_nonnull_30_11) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public static method Get(): Task<int> "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    tcs.SetResult(1); "
        "    return tcs.Task; "
        "  end Get; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReturnDiag));
}

// Twin: `OwningTaskCompletionSource<T>` (Pattern B), both spellings.
BEZEL_TEST(tcs_task_borrow_escape_8616, reject_return_owning_source_task_30_11) {
    auto r = runAot(
        "module M; "
        "public class Item public property V: int; end Item; "
        "public class C "
        "  public static method Get(): OwningTask<Item> "
        "    using var tcs := new OwningTaskCompletionSource<Item>(); "
        "    return tcs.Task; "
        "  end Get; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReturnDiag));
}

BEZEL_TEST(tcs_task_borrow_escape_8616,
           reject_return_owning_source_nullcond_task_30_11) {
    auto r = runAot(
        "module M; "
        "public class Item public property V: int; end Item; "
        "public class C "
        "  public static method Get(): OwningTask<Item>? "
        "    using var tcs := new OwningTaskCompletionSource<Item>(); "
        "    return tcs?.Task; "
        "  end Get; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReturnDiag));
}

// Twin: the non-generic `TaskCompletionSource` (Pattern A, no result).
BEZEL_TEST(tcs_task_borrow_escape_8616, reject_return_nongeneric_source_task_30_11) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public static method Get(): Task "
        "    using var tcs := new TaskCompletionSource(); "
        "    tcs.SetResult(); "
        "    return tcs.Task; "
        "  end Get; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kReturnDiag));
}

// Twin: a field store of the borrow (§28.3's field-store bullet) — the same
// "is this a borrow" gate decides it.
BEZEL_TEST(tcs_task_borrow_escape_8616, reject_field_store_task_30_11) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  private _t: Task<int>?; "
        "  public method Fill(): int "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    tcs.SetResult(1); "
        "    _t := tcs.Task; "
        "    return 0; "
        "  end Fill; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFieldDiag));
}
