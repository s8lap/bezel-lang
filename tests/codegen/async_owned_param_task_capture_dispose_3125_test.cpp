// Codegen / disposal — fix #3125 (second half): a state-machine frame released
// an `owned` Task-family PARAMETER through class-vtable dispatch.
//
// Spec (revision v0.254):
//   §30.1 "Async methods" — an `owned` parameter the body never moves out is
//     disposed by the FRAME when the task reaches its terminal state; §29.8
//     states the iterator twin for the two enumerator frames.
//   §30.5 "Task disposal" — disposing a task is ABANDONMENT: the operation runs
//     to its terminal state unsupervised and its result, if owned, is disposed
//     on arrival. That is `emitTaskDisposal`, NOT a vtable call.
//
// THE DEFECT. All three frame-owning emitters' auto-drop loops — the plain
// async method's `emitAsyncOwnedParamAutoDispose`, the async iterator's Dispose
// sweep (both LlvmBackend_Async.cpp) and the sync iterator's owned-param sweep
// (LlvmBackend_Iterators.cpp) — called `emitDisposeAndReclaim` unconditionally.
// That loads a vtable pointer out of the object and calls through it. A
// `bz_task_t*` is a runtime struct with no Bezel vtable, so the load read task
// bytes and the indirect call jumped into garbage.
//
// PROVEN FAILS-WITHOUT, mechanically, on this base (bezelc @ 701cb2d, no ASan
// needed — the fault is unconditional): each of the three programs below
// compiled cleanly and then died with SIGSEGV (exit 139, core dumped). With the
// fix each returns its expected value. One test per suspect emitter, which is
// the whole population that shares the auto-drop loop.
//
// THE FIX is one routing decision, not three: `emitOwnedParamCaptureDispose`
// (LlvmBackend_Disposal.cpp) asks `taskFamilyTypeFromParam` and sends a
// Task-family capture to `emitTaskDisposal` and everything else to
// `emitDisposeAndReclaim`. All three loops call it, so they cannot drift — the
// failure mode that produced this bug in the first place.
//
// The last test is the FALSE-FLIP CONTROL: an `owned` parameter of an ordinary
// disposable CLASS must still take the vtable route. Under `runAotChecked`
// LeakSanitizer fails the test if the routing change silenced that disposal.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ── FAILS WITHOUT THE FIX (SIGSEGV at 701cb2d) ──────────────────────────────

// The plain async frame. `SinkAsync` takes `owned task` and never discharges it
// — no chaining, no `return await` — so `emitAsyncOwnedParamAutoDispose` is the
// releasing path. // per §30.1 / §30.5, v0.254
BEZEL_TEST(async_owned_param_task_capture_dispose_3125,
           plain_async_frame_owned_task_param_abandoned_not_vtabled) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static async method FetchAsync(): Task<int> "
        "    return 7; "
        "  end FetchAsync; "
        "  public static async method SinkAsync(owned task: Task<int>): Task<int> "
        "    return 5; "
        "  end SinkAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.SinkAsync(C.FetchAsync()); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(5, r.exitCode);
}

// ── WHAT THIS FIX DOES NOT COVER (both measured, both still open) ──────────
//
// 1. #3125's FIRST half — `return await <owned parameter>` is still not
//    recognised as the move-out §30.5 says it is, so the frame auto-drop runs on
//    a parameter the body already consumed. The routing fix above is orthogonal
//    and does not help: with it applied, `ChainAsync(owned task: Task<Cfg>)`
//    whose body is `return await task;` disposes the awaited RESULT and the
//    caller then reads it —
//        AddressSanitizer: heap-use-after-free, READ of size 4
//          #0 bz$async$Main$Resume
//          freed by bz$elemDispose <- bz$M.C$ChainAsync
//          allocated by bz$async$FetchAsync$Resume
//    — SIGSEGV without a sanitizer. Deliberately NOT pinned here: a test that
//    asserted today's behaviour would pin a use-after-free, and one that
//    asserted the correct value would be a known-failing test in the suite.
//    #3125 stays OPEN for it, which is why this commit does not close it.
//
// 2. #3157 — the router has one arm and a wrong default. An `owned` parameter of
//    any NON-Task vtable-less built-in (`Random`, `Channel<T>`, `StringBuilder`,
//    …) still reaches `emitDisposeAndReclaim` and still SEGVs in
//    `bz$async$SinkAsync$Resume`, measured with this fix applied.

// The SYNC iterator frame (LlvmBackend_Iterators.cpp). `Gen`'s `owned t` is
// never discharged, so the enumerator's Dispose runs the same loop. 1 + 2 = 3.
// // per §29.8 / §30.5, v0.254
BEZEL_TEST(async_owned_param_task_capture_dispose_3125,
           sync_iterator_frame_owned_task_param_abandoned_not_vtabled) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static async method FetchAsync(): Task<int> "
        "    return 7; "
        "  end FetchAsync; "
        "  public static method Gen(owned t: Task<int>): IEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    for var x in C.Gen(C.FetchAsync()) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

// The ASYNC iterator frame (the Dispose sweep in LlvmBackend_Async.cpp) — the
// third emitter, reached only through `await for`. 1 + 2 = 3.
// // per §29.8 / §30.5, v0.254
BEZEL_TEST(async_owned_param_task_capture_dispose_3125,
           async_iterator_frame_owned_task_param_abandoned_not_vtabled) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static async method FetchAsync(): Task<int> "
        "    return 7; "
        "  end FetchAsync; "
        "  public static async method Gen(owned t: Task<int>): IAsyncEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in C.Gen(C.FetchAsync()) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

// ── THE FALSE-FLIP CONTROL (passes with and without the fix) ────────────────

// An `owned` parameter of an ordinary disposable CLASS must keep taking the
// vtable route: `emitOwnedParamCaptureDispose` is a router, and a router that
// sent everything to `emitTaskDisposal` would leak every non-task capture
// instead. `Res` is heap-allocated, so a frame that failed to release it is a
// LeakSanitizer report and `r.ran` goes false. // per §28.3 / §30.1, v0.254
BEZEL_TEST(async_owned_param_task_capture_dispose_3125,
           plain_async_frame_owned_class_param_still_vtable_disposed_control) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Res; "
        "class C "
        "  public static async method SinkAsync(owned r: Res): Task<int> "
        "    return 5; "
        "  end SinkAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.SinkAsync(new Res(7)); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(5, r.exitCode);
}
