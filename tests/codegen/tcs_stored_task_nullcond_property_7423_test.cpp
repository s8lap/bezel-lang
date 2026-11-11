// Codegen — issue #7423 (compiler ABORT): a `using var` over a
// NULL-CONDITIONAL `?.` read of a stored-task source's `Task` property emits
// MALFORMED IR, and the compiler aborts.
//
// Spec basis (v0.357):
//   §7.5 (Null-conditional access `?.`): "`x?.M` returns null if `x` is null;
//     otherwise it performs the access. **The result type is always
//     nullable**."
//   §30.11 (`TaskCompletionSource<T>` / `OwningTaskCompletionSource<T>`):
//     "The **source owns the task object** its `Task` property exposes. A read
//     of the property is a **borrow** of the task, sourced at the source
//     object's location ... and is admitted wherever §30.5 admits a task
//     borrow with that source ... **there is no first-read ownership
//     transfer.**" And: "For either kind, a **never-awaited** task is disposed
//     exactly once, by the source at source disposal."
//   §28.10 (`using var` initialized from a borrow): over a borrow "the `using
//     var` does **not** assume a disposal obligation (derives: K2 — no
//     obligation can be assumed from a borrow): at scope end, no `Dispose()` is
//     called, because the value is not owned by this scope."
//   §28.12 (asynchronous disposal): `await using var` takes §28.10's
//     admission unchanged.
//   §7.8 (null-forgiving `!`): "`x!` asserts non-nullness. If `x` is null at
//     runtime, `NullReferenceException` is raised."
//   §33.3 (exit code): "an exception escaping `Main` ... produces exit code
//     **70**".
//
// THE DEFECT. `tcs?.Task` is a NullCondMemberExpr — a DISTINCT AST node — and
// neither spelling of the member had a `?.` twin:
//
//   * sema's `resolveMemberAccess` types the `.` spelling of the stored-task
//     `Task` property by hand (the §6.6 / #974 arm) but `resolveNullCondMember`
//     had no such arm, so the `?.` node was left UNTYPED — no diagnostic, no
//     type;
//   * codegen's `lowerExpr` reads the `.` spelling through the `bz_tcs_task`
//     call (the same #974 arm) but its `NullCondMemberExpr` arm had no twin,
//     so the access lowered to NOTHING.
//
// The local therefore kept a ZERO-INITIALISED slot (codegen's #2434
// "lowerLocalDecl's initializer expression failed to lower to a value" warning)
// — and the `await` then loaded that i32 and passed it to `bz_task_is_done` /
// `bz_task_then`, which take a POINTER:
//
//   Call parameter type does not match function signature!
//     %13 = load i32, ptr %3, align 4
//    ptr  %await.done = call i1 @bz_task_is_done(i32 %13)
//   bezelc: codegen error: IR verification failed (malformed IR emitted; see
//   report above)
//
// MEASURED on the base (r30-c1 e41796df3), AOT:
//   using var t := tcs?.Task;   -> bezelc ABORTS (IR verification failed)
//   var      t := tcs?.Task;    -> bezelc ABORTS (IR verification failed)
//   using var t := tcs.Task;    -> rc=77 (the plain spelling was correct)
//
// So the `using var` is NOT the discriminator on this tree — the `?.` READ was
// unlowerable in every binding form. The program is VALID and must run:
// §30.11 makes the property read a borrow of the source's storage, §28.10
// admits a `using var` over a borrow (as a pin that disposes nothing), and
// the source's disposal is the task's single disposal.
//
// THE ORACLE is the HARNESS verdict, not the exit code: a compile reject, a
// sanitizer report, a crash, or a run that never terminates all surface as
// ran=false and collapse to the -999 sentinel. Pre-fix the shape is a compile
// REJECT (the malformed-IR abort), post-fix it compiles and exits 77.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The oracle is the HARNESS verdict: a compile reject, a sanitizer report, a
// crash, or a run that never terminates all surface as ran=false -> -999.
int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// #7423 — the ISSUE'S OWN REPRO, cell (i): a `using var` over the
// null-conditional read of a stored-task source's `Task` property.
//
// §30.11: the read is a BORROW of the source's task; §28.10: the `using var`
// over a borrow assumes NO disposal obligation, so the SOURCE disposes the
// task exactly once. The program must therefore compile and exit 77.
//
// Fails without the fix: bezelc aborts with "IR verification failed (malformed
// IR emitted)" -> the compile is a reject -> ran=false -> -999 != 77.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_nullcond_7423,
           using_var_over_nullcond_stored_task_property_exits_77_spec_30_11) {
    // per §7.5 / §30.11 / §28.10 (v0.357)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    using var t := tcs?.Task; "
        "    _ := tcs.TrySetResult(77); "
        "    return await t!; "
        "  end Main; "
        "end C;"), 77);
}

// ---------------------------------------------------------------------------
// #7423 — the ISSUE'S SECOND CELL: the same program with a NULLABLE
// receiver (`var tcs: TaskCompletionSource<int>? := null;`).
//
// §7.5: "`x?.M` returns null if `x` is null" — the null receiver yields a null
// task; §7.8: "`x!` asserts non-nullness. If `x` is null at runtime,
// `NullReferenceException` is raised"; §33.3: an exception escaping `Main`
// produces exit code 70.
//
// The point of the row is that the program COMPILES and RUNS: on the base the
// compiler aborted with malformed IR on this cell too.
//
// Fails without the fix: the compile is a reject -> ran=false -> -999 != 70.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_nullcond_7423,
           nullcond_stored_task_property_over_a_nullable_receiver_raises_null_reference_spec_7_8) {
    // per §7.5 / §7.8 / §33.3 (v0.357)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var tcs: TaskCompletionSource<int>? := null; "
        "    using var t := tcs?.Task; "
        "    _ := tcs?.TrySetResult(77); "
        "    return await t!; "
        "  end Main; "
        "end C;"), 70);
}

// ---------------------------------------------------------------------------
// #7423 — the plain-`var` twin of the issue's repro. It aborted on the base
// too (MEASURED), so it is a SECOND row of the same defect, not a control.
//
// §30.11: the read is a borrow; §28.3: "storage lends" — a plain `var` over a
// storage read owns no frame, so nothing is disposed for the local and the
// source's disposal is the task's single disposal.
//
// Fails without the fix: the compile is a reject -> ran=false -> -999 != 77.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_nullcond_7423,
           plain_var_over_nullcond_stored_task_property_exits_77_spec_30_11) {
    // per §30.11 / §28.3 (v0.357)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    var t := tcs?.Task; "
        "    _ := tcs.TrySetResult(77); "
        "    return await t!; "
        "  end Main; "
        "end C;"), 77);
}

// ---------------------------------------------------------------------------
// #7423 — CONTROL (the non-`?.` spelling): `using var t := tcs.Task;`.
//
// This row was ALREADY correct on the base (rc=77, the #6194 borrow-pin), and
// it must STAY correct: the fix must not disturb the plain spelling of the
// same property. This is the admitting twin that shows the shape is admitted
// rather than rejected for an unrelated reason.
//
// A control row that stays green on the base is doing its job.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_nullcond_7423,
           using_var_over_plain_stored_task_property_still_exits_77_spec_30_11) {
    // per §30.11 / §28.10 (v0.357)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    using var t := tcs.Task; "
        "    _ := tcs.TrySetResult(77); "
        "    return await t!; "
        "  end Main; "
        "end C;"), 77);
}

// ---------------------------------------------------------------------------
// #7423 — the `await using var` spelling of the issue's repro. §28.12 takes
// §28.10's admission unchanged, and over a BORROW right-hand side the
// binding assumes no obligation, so scope exit disposes nothing and awaits
// nothing.
//
// Fails without the fix: the compile is a reject -> ran=false -> -999 != 77.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_nullcond_7423,
           await_using_var_over_nullcond_stored_task_property_exits_77_spec_28_12) {
    // per §7.5 / §30.11 / §28.12 (v0.357)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    await using var t := tcs?.Task; "
        "    _ := tcs.TrySetResult(77); "
        "    return await t!; "
        "  end Main; "
        "end C;"), 77);
}

// ---------------------------------------------------------------------------
// #7423 — the NEVER-AWAITED shape: §30.11 "a **never-awaited** task is
// disposed exactly once, by the source at source disposal."
//
// This is the row that pins the BORROW-PIN: if the `using var` over the
// null-conditional read registered a scope-end task disposal of its own, the
// source's own release would then read the SAME inner task again — the
// double-free #6194 measured on the plain spelling. The ASan/LSan oracle
// nets both a double free and a leak.
//
// The row READS the handle (`t?.IsCompleted`), so it is not a constant: on the
// base the `?.` read lowered to NOTHING and the local kept a zero-initialised
// slot, so `done` was null and the row printed "False"; with the fix the
// handle is the source's real task, `done` is true, and the row prints
// "True".
//
// Fails without the fix: the row prints "False" instead of "True".
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_nullcond_7423,
           never_awaited_nullcond_stored_task_is_disposed_once_by_the_source_spec_30_11) {
    // per §7.5 / §30.11 / §28.10 / §30.3 (v0.357)
    JitResult r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    using var t := tcs?.Task; "
        "    _ := tcs.TrySetResult(77); "
        "    var done: bool? := t?.IsCompleted; "
        "    Console.WriteLine(done ?? false); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// ---------------------------------------------------------------------------
// #7423 — the OwningTaskCompletionSource<T> twin: the same stored-task
// property, the other source class. §30.11's rule is stated for "either
// source", so the borrow-pin must hold for both.
//
// Like the row above, it READS the handle, so it discriminates.
//
// Fails without the fix: the row prints "False" instead of "True".
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_tcs_stored_task_nullcond_7423,
           owning_tcs_nullcond_stored_task_property_is_a_borrow_pin_spec_30_11) {
    // per §7.5 / §30.11 / §28.10 / §30.3 (v0.357)
    JitResult r = runAotChecked(
        "module M; "
        "class Res "
        "  public constructor() end constructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    using var tcs := new OwningTaskCompletionSource<Res>(); "
        "    using var t := tcs?.Task; "
        "    _ := tcs.TrySetResult(new Res()); "
        "    var done: bool? := t?.IsCompleted; "
        "    Console.WriteLine(done ?? false); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}
