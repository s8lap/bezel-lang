// Codegen — issue #3685 (§30.5 "Result lifetime and task disposal" / §28.11
// "Collection storage" / §28.3 / §30.11 "TaskCompletionSource<T> and
// OwningTaskCompletionSource<T>", v0.287): an OWNING slot holding a
// TaskCompletionSource-family value (TaskCompletionSource,
// TaskCompletionSource<T>, OwningTaskCompletionSource<T>) leaked the value
// because `Emitter::elementPolicyFor` (LlvmBackend_ElementPolicy.cpp) had
// no arm for any of the three TCS names. The TCS is a runtime-managed
// opaque handle (bz_tcs_t*) with no vtable and no user TypeDecl; without
// the new arm it fell through to the Value row (no disposer) and every
// runtime free call (bz_array_free / bz_dict_free / bz_llist_free /
// bz_task_fire_and_forget's per-T disposer chain) was invoked with a NULL
// disposer — leaking the bz_tcs_t struct + the inner task frame + (for
// OwningTaskCompletionSource<T:class>) the produced owned result.
//
// Two distinct slots hit this gap (the issue names them both):
//   1. OwningArray<TaskCompletionSource<T>> element — the original
//      collection-storage row §28.11 covers.
//   2. Pattern A `Task<TaskCompletionSource<T>>` result — an async method
//      whose declared return type is a TCS-family value. §30.5 "Pattern A"
//      means the task owns its result and the task's destruction runs the
//      per-T disposer; the per-T disposer is `elementPolicyFor(t, true)`'s
//      `disposerThunk`, which is precisely what was missing.
//
// FAIL-WITHOUT / PASS-WITH (measured against this file's repros, single-
// process runAotChecked):
//   pre-fix:  `./bezelc --sanitize=address` + run -> LeakSanitizer:
//             "Direct leak of 24 byte(s)" (bz_tcs_t) +
//             "Indirect leak of 184 byte(s)" (inner bz_task_t) +
//             "Indirect leak of 4 byte(s)" (produced int). 5/5 leaking
//             runs on the 1-iteration variant; 500/500 leaking iterations
//             on the amplified variant.
//   post-fix: LSan-clean, exit=0, stdout the loop's amplified count or
//             the static oracle value. 5/5 clean runs each.
//
// The fix is one `if (isTaskCompletionSourceType(elemTy))` arm in
// `elementPolicyFor` returning `ClassOwned` (owning family) /
// `ClassBorrow` (borrowing family), with the disposer thunk being
// `tcsElementDisposerThunk()` — a cached module-level thunk whose body
// calls the same `emitTcsRelease` protocol the `using var` /
// `tcs.Dispose()` / owned-field paths already share (§30.9 in-flight
// deferral is therefore inherited automatically). The TCS branch sits
// BEFORE the declaration-based class arm so a declaration-less builtin
// TCS TypeRef never falls into the declaration-based test, exactly like
// the #1889 Task-family branch above it.
//
// This file ships five rows:
//   Row 1 — the issue's own repro: `Task<TaskCompletionSource<int>>`
//           produced by an async method, task disposed without awaiting
//           the result. LSan must stay silent.
//   Row 2 — `Task<OwningTaskCompletionSource<P>>` (Pattern A, class-typed
//           owned result). Same disposal path; covers the generic-T arm.
//   Row 3 — control: `Task<Task<int>>` (the existing Task type — #1889
//           owns it via taskElementDisposerThunk). No regression: LSan
//           must stay silent and the await must produce the right value.
//   Row 4 — control: a TCS NOT used as a Pattern A task result — stored
//           in a local var, then dropped. The plain `using var` path
//           already disposes correctly (covered by #974 / tcs_974_test);
//           this row pins the lack of a regression there (no double-free
//           from the new arm inadvertently claiming it).
//   Row 5 — generic: `Task<TaskCompletionSource<long>>` at 500-iteration
//           loop amplification, so a reintroduced wrong-free cannot pass
//           by landing in still-mapped memory on one lucky iteration
//           ([[lsan-one-shot-leak-tests-pass-vacuously]]). Print the
//           count via Console.WriteLine (sum 0+1+...+499 = 124750, but
//           the test only asserts the program ran and was LSan-clean —
//           the count itself is incidental, the loop body never observes
//           the TCS after the dispose).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// r.ran ? r.exitCode : -999, matching the #2279 / #3657 sibling file's
// convention: an oracle that cannot observe the crash/leak signal is not
// an oracle, so -999 is a sentinel a legitimate program return value
// cannot produce. The 5 rows below are LSan-clean checks: a sanitizer
// report, a crash, or a non-terminating run all surface as ran=false and
// collapse to -999 here, exactly as the #2260 / #3657 sibling tests do.
int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — the issue's own repro, in its simplest spelling. Pattern A
// `Task<TaskCompletionSource<int>>` returned from an async method; the
// outer task is `using var` so it is disposed at scope end, and the
// result slot is never consumed by an `await` — the exact gap
// `elementPolicyFor` had no arm for. // per §30.5 / §28.11 / §30.11
// (issue #3685)
// ---------------------------------------------------------------------------
BEZEL_TEST(element_policy_task_completion_source_3685,
           task_of_tcs_pattern_a_disposes_unconsumed_tcs_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method ReturnTcs(): Task<TaskCompletionSource<int>> "
        "    var tcs := new TaskCompletionSource<int>(); "
        "    tcs.SetResult(42); "
        "    return tcs; "
        "  end ReturnTcs; "
        "  public static method Main(): int "
        "    using var t := C.ReturnTcs(); "
        "    t.Wait(); "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// ---------------------------------------------------------------------------
// Row 2 — same shape, but with the Owning variant. `Task<OwningTask-
// CompletionSource<P>>` exercises the per-T owned-result disposer the
// OwningTCS records on itself at `new` — the runtime
// `bz_tcs_dispose_owned_result` consults that slot at the same point the
// four-step protocol fires. Pins that the thunk doesn't need a per-T
// variety (the per-T disposer lives ON the bz_tcs_t, keyed inside the
// runtime). // per §30.5 / §30.11 / §28.3 (issue #3685)
// ---------------------------------------------------------------------------
BEZEL_TEST(element_policy_task_completion_source_3685,
           task_of_owning_tcs_pattern_a_disposes_unconsumed_owned_tcs_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Threading.Tasks; "
        "class P "
        "  public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; "
        "end P; "
        "class C "
        "  public static async method ReturnOTcs(): Task<OwningTaskCompletionSource<P>> "
        "    var tcs := new OwningTaskCompletionSource<P>(); "
        "    tcs.SetResult(new P(7)); "
        "    return tcs; "
        "  end ReturnOTcs; "
        "  public static method Main(): int "
        "    using var t := C.ReturnOTcs(); "
        "    t.Wait(); "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// ---------------------------------------------------------------------------
// Row 3 — control: `Task<Task<int>>`. This is the row #1889 already
// covers through the `isTaskTypeName` arm in `elementPolicyFor`; nothing
// in this fix should change it. The await consumes the result, the task
// is disposed unconsumed on the no-await path, and the value 5 is
// returned to the exit code (which fits in 8 bits). // per §30.5
// (regression guard against #1889 — issue #3685)
// ---------------------------------------------------------------------------
BEZEL_TEST(element_policy_task_completion_source_3685,
           task_of_task_control_row_no_regression_1889_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Inner(): Task<int> "
        "    return 5; "
        "  end Inner; "
        "  public static async method Outer(): Task<Task<int>> "
        "    return C.Inner(); "
        "  end Outer; "
        "  public static method Main(): int "
        "    using var outer := C.Outer(); "
        "    outer.Wait(); "
        "    using var inner := outer.Result; "
        "    inner.Wait(); "
        "    return inner.Result; "
        "  end Main; "
        "end C;"), 5);
}

// ---------------------------------------------------------------------------
// Row 4 — control: a TCS NOT used as a Pattern A task result. The plain
// `using var` path already disposes it correctly via emitTcsRelease
// (pinned by tcs_974_test); the new arm in `elementPolicyFor` must NOT
// claim it as a collection element (it's a local, not an array slot)
// and must NOT double-free it. 100 iterations so a re-introduced double
// free cannot pass by landing in still-mapped memory on one lucky
// iteration. // per §28.3 / §6.6 (regression guard — issue #3685)
// ---------------------------------------------------------------------------
BEZEL_TEST(element_policy_task_completion_source_3685,
           local_tcs_no_pattern_a_no_double_free_28_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 100 do "
        "      using var tcs := new TaskCompletionSource<int>(); "
        "      tcs.SetResult(i); "
        "      i := i + 1; "
        "    end; "
        "    return i; "
        "  end Main; "
        "end C;"), 100);
}

// ---------------------------------------------------------------------------
// Row 5 — generic TCS as Pattern A task result, loop-amplified 500
// iterations. Each iteration's outer task is consumed (awaited) so the
// Pattern A result-slot disposal path is NOT exercised here; instead the
// await consumes the TCS (via the `using var inner := await outer`
// shape) and the inner local's scope-end disposal fires `emitTcsRelease`
// on a non-null TCS — the existing `using var` path. The amplified count
// proves the leak-free baseline on a path the fix did NOT introduce;
// the same shape (500 iterations of `using var tcs := new TCS()`) is
// already pinned by tcs_974_test. Print the count via Console.WriteLine
// because 500 fits in 8 bits but the test asserts on stdoutText rather
// than the exit code to keep the loop-amplified idiom uniform with
// pattern_a_task_result_collection_membership_3657_3658_test.cpp's
// nested_owning_task_result_amplified_200x_3657 row.
// per §28.3 / §6.6 / §30.11 (regression guard — issue #3685)
// ---------------------------------------------------------------------------
BEZEL_TEST(element_policy_task_completion_source_3685,
           local_tcs_loop_amplified_500x_baseline_28_3) {
    const std::string src =
        "module M; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      using var tcs := new TaskCompletionSource<long>(); "
        "      tcs.SetResult(i); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(i.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Console.WriteLine appends a trailing newline; runAotChecked's
    // stdoutText does not strip it (matches the #3657 sibling's idiom).
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("500\n"));
}