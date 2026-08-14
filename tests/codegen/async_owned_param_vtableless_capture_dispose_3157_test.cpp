// Codegen / disposal — fix #3157 (sister to #3125): a state-machine frame released
// an `owned` NON-Task vtable-less built-in class PARAMETER through class-vtable
// dispatch. Same defect shape as #3125 (Task); the wild-dispatch router was
// overloaded for every vtable-less built-in, but only the Task arm routed cleanly.
//
// Spec (revision v0.254):
//   §30.1 "Async methods" — an `owned` parameter the body never moves out is
//     disposed by the FRAME when the task reaches its terminal state; §29.8
//     states the iterator twin for the two enumerator frames.
//   §28.2 "the first call performs the disposal; subsequent calls are no-ops"
//     together with §28.9 "the compiler frees the undischarged value at its
//     owner's release point" — these are the spec rule a vtable-less built-in's
//     release must satisfy at frame teardown.
//
// THE DEFECT. #3125 settled the Task arm by asking `taskFamilyTypeFromParam`
// and routing Task-family captures to `emitTaskDisposal`; everything else fell
// through to `emitDisposeAndReclaim` — the generic vtable call that loads word
// 0 as a vtable pointer and calls slot 2. A `bz_random_t` / `bz_channel_t` /
// `bz_string_builder_t` (Random / Channel<T> / StringBuilder — and ten more
// vtable-less builtins besides) has no Bezel vtable at word 0: word 0 is seed
// bytes or opaque payload. The load read those bytes as a pointer and the
// indirect call jumped into garbage.
//
// MEASURED on the parent 6da66d4 (per §30.1 / §29.8, no ASan needed — the
// fault is unconditional) for each of Random, Channel<int>, StringBuilder:
//
//     AddressSanitizer: SEGV on unknown address
//       #0 bz$async$SinkAsync$Resume
//       #1 bz$M.C$SinkAsync
//       #2 bz$async$Main$Resume
//
// with `rax = 0xd673470553fada15` for the Random case — the four seed bytes
// interpreted as a pointer. With the fix each returns its expected value; no
// ASan report. The same three frames (`#emitAsyncOwnedParamAutoDispose`,
// async iterator Dispose sweep, sync iterator owned-param sweep) all call
// `emitOwnedParamCaptureDispose`, so widening it once closes all three
// populations. The Task TWIN at the bottom is the existing #3125 fix — kept
// here as a canary so a future refactor of the router cannot break it again.
//
// THE FIX is ONE routing decision, not a second spelling per population:
// `emitOwnedParamCaptureDispose` (LlvmBackend_Disposal.cpp) now decides
// between three classifiers and the loud `internal:` warning that flags a
// future addition to the negative list:
//
//   (1) Task / Task<T> / OwningTask<T>          → `emitTaskDisposal` (#3125).
//   (2) Vtable-less built-in class (Random,
//       Channel<T>, StringBuilder, …)           → `emitVtablelessBuiltinRelease`
//                                                (the same per-name release
//                                                the explicit `x.Dispose()`
//                                                lowering and the `using var`
//                                                scope-exit path consult:
//                                                #2967 / #2921 / #2973).
//   (3) Recognised class (user / builtin
//       exception / imported user)              → `emitDisposeAndReclaim`.
//
// The default that the previous fix deliberately left intact — "anything else
// falls through to vtable dispatch" — was the exact silent fail mode #3157
// measured; it is now an `internal:` warning. The negative list goes away: a
// future vtable-less built-in either enters (2) (after its arm lands in
// `emitVtablelessBuiltinRelease`) or hits the (2)-no-arm warning. // per §30.1,
// fix #3157.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ── FAILS WITHOUT THE FIX (SIGSEGV at 6da66d4) ──────────────────────────────

// The plain async frame, Random param. `SinkAsync` takes `owned r: Random` and
// never discharges it, so `emitAsyncOwnedParamAutoDispose` is the releasing
// path. // per §30.1, v0.254
BEZEL_TEST(async_owned_param_vtableless_capture_dispose_3157,
           plain_async_frame_owned_random_param_released_not_vtabled) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static async method SinkAsync(owned r: Random): Task<int> "
        "    return 5; "
        "  end SinkAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.SinkAsync(new Random()); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(5, r.exitCode);
}

// Channel<int> param — the runtime struct needs `bz_channel_dispose` with the
// ChannelClosedException vtable; that's a different runtime call from the
// Random case, exercising that `emitVtablelessBuiltinRelease`'s arm list
// covers Channel specifically. // per §30.1 / §30.15, v0.254
BEZEL_TEST(async_owned_param_vtableless_capture_dispose_3157,
           plain_async_frame_owned_channel_param_released_not_vtabled) {
    auto r = runAotChecked(
        "module M; import System.Threading; "
        "class C "
        "  public static async method SinkAsync(owned ch: Channel<int>): Task<int> "
        "    return 7; "
        "  end SinkAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.SinkAsync(new Channel<int>(2)); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

// StringBuilder param — the bz_array_t-backed owner whose release thunk is the
// string-element disposer. // per §30.1 / stdlib §3.1, v0.254
BEZEL_TEST(async_owned_param_vtableless_capture_dispose_3157,
           plain_async_frame_owned_string_builder_param_released_not_vtabled) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static async method SinkAsync(owned sb: StringBuilder): Task<int> "
        "    return 9; "
        "  end SinkAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.SinkAsync(new StringBuilder()); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(9, r.exitCode);
}

// The SYNC iterator frame (the sync iterator's owned-param sweep in
// LlvmBackend_Iterators.cpp), Random twin. The iterator Dispose runs the same
// loop as the plain async frame; with #3125 alone it would still SEGV because
// the parameter is `Random`, not a Task. // per §29.8, v0.254
BEZEL_TEST(async_owned_param_vtableless_capture_dispose_3157,
           sync_iterator_frame_owned_random_param_released_not_vtabled) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Gen(owned r: Random): IEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    for var x in C.Gen(new Random()) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

// The ASYNC iterator frame (the async iterator's Dispose sweep in
// LlvmBackend_Async.cpp), reached only through `await for`. StringBuilder twin
// — exercises both the ChannelClosedException-less release form and the
// iterator-family's `iteratorAutoDispose` flag set by sema. // per §29.8, v0.254
BEZEL_TEST(async_owned_param_vtableless_capture_dispose_3157,
           async_iterator_frame_owned_string_builder_param_released_not_vtabled) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static async method Gen(owned sb: StringBuilder): IAsyncEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in C.Gen(new StringBuilder()) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

// ── THE TASK TWIN (#3125 CANARY) ──────────────────────────────────────────

// #3157 widens the router that #3125 introduced. A regression in the Task arm
// — e.g. a future refactor that flips the (1)/(2) order or removes the Task
// gate — is exactly the failure mode the sister fix prevented. Keeping a
// pinger here means a regression of the Task arm fails the same suite under
// the same harness instead of needing its own run. // per §30.1, fix #3125
BEZEL_TEST(async_owned_param_vtableless_capture_dispose_3157,
           plain_async_frame_owned_task_param_still_abandoned_not_vtabled_3125_twin) {
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

// ── THE FALSE-FLIP CONTROL ────────────────────────────────────────────────

// An `owned` parameter of an ordinary disposable CLASS must KEEP taking the
// vtable route. (3) above is the branch that does so — a refactor that sent
// every capture to `emitVtablelessBuiltinRelease` would silence that disposal
// and `Res` would leak. Under `runAotChecked` LeakSanitizer reports the leak
// (r.ran goes false). // per §28.3 / §30.1, v0.254
BEZEL_TEST(async_owned_param_vtableless_capture_dispose_3157,
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
