// Codegen — issue #7705 (§28.12, v0.350): an `await using var` binding awaits
// its `DisposeAsync` on the EXCEPTION edge, not only on the normal one.
//
//     await using var o := new NOK();
//     throw new InvalidOperationException("boom");     // or any throwing call
//
// §28.12, v0.350, verbatim: asynchronous cleanup runs "where the program wrote
// `await using` (whose implicit `finally` awaits `DisposeAsync` on both the
// normal and exception edges of an async body ...)". And step 1: "For `await
// using`, the awaited `DisposeAsync()` performs whatever asynchronous cleanup
// the class defines; the destructor body (if any) is **not** also run, because
// `DisposeAsync` supersedes it".
//
// The unwind pass took the synchronous fallback on every exception edge
// (`emitDisposals(isUnwind=true)` passed no DisposeAsync), so the binding's
// DISPOSER BODY ran and `DisposeAsync` never did (MEASURED on origin/main
// 26216e9f: every row below prints `oda=0 odt=100`). Now an unwind pass emitted
// in the async body's own resume function suspends: the in-flight exception is
// parked across the await (the thread's slot is clear while other tasks run and
// while DisposeAsync's own body executes), restored on success, and superseded by
// the NEW exception when DisposeAsync itself faults (§28.4 / §31.2: the later
// exception supersedes; every remaining disposal still runs).
//
// The Disposer: `NOK.DisposeAsync` awaits `Task.Delay(0)`, which really
// suspends, and counts `oda`; its destructor counts `odt`. `NDLY` / `NDFAULT`
// and the `NT*` heads await `Task.Delay(1)`, so the body resumes on whichever
// worker the timer wakes: the bound-task and owned-task rows below need that
// cross-thread resume. Every row loops 100x under generated-code ASan
// (runAotChecked) and asserts: the exception still reaches the catch, `oda` ==
// iterations, `odt` == 0, the owned `Cell` was cascaded (`cells=0`).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr const char* kPrelude = R"BZ(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class K
  public static oda: int := 0;
  public static odt: int := 0;
  public static caught: int := 0;
  public static other: int := 0;
  public static sawBeforeFinally: int := 0;
  public static fin: int := 0;
  public constructor() end constructor;
end K;
public class NOK: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NOK;
public class NIN: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NIN;
public class NSYNC: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public method DisposeAsync(): Task
    K.oda := K.oda + 1;
    throw new ArgumentException("sboom");
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NSYNC;
public class NFAULT: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    K.oda := K.oda + 1;
    throw new ArgumentException("fboom");
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NFAULT;
public class NDLY: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(1);
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NDLY;
public class NDFAULT: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(1);
    K.oda := K.oda + 1;
    throw new ArgumentException("fboom");
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NDFAULT;
public class NTDLY: IAsyncDisposable
  public owned t: Task;
  public owned _c: Cell;
  public constructor(owned tk: Task) t := tk; _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(1);
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NTDLY;
public class NTIN: IAsyncDisposable
  public owned t: Task;
  public owned _c: Cell;
  public constructor(owned tk: Task) t := tk; _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NTIN;
public class NTRE: IAsyncDisposable
  public owned t: Task;
  public owned _c: Cell;
  public constructor(owned tk: Task) t := tk; _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(1);
    K.oda := K.oda + 1;
    await t;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NTRE;
public class NTF: IAsyncDisposable
  public owned t: Task;
  public owned _c: Cell;
  public constructor(owned tk: Task) t := tk; _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(1);
    K.oda := K.oda + 1;
    throw new ArgumentException("fboom");
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NTF;
public class Program
  public static method MkI(): owned IAsyncDisposable return new NOK(); end MkI;
  public static method Tally(m: string, want: string)
    if m = want then K.caught := K.caught + 1; else K.other := K.other + 1; end;
  end Tally;
  public static method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;
  public static async method BoomAsync(): Task
    await Task.Delay(0);
    throw new InvalidOperationException("boom");
  end BoomAsync;
  public static async method BoomLater(): Task
    await Task.Delay(1);
    throw new InvalidOperationException("boom");
  end BoomLater;
)BZ";

// `body` is the body of R(); Main runs it 100 times and tallies where the
// exception went. `want` is the message the catch must see.
std::string build(const char* body, const char* want,
                  const char* extraMembers = "") {
    return std::string(kPrelude) + extraMembers +
           "  public static async method R(): Task\n" + body +
           "  end R;\n" + R"BZ(
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R();
      catch var e: InvalidOperationException =>
        Program.Tally(e.Message, ")BZ" + want + R"BZ(");
      catch var e: ArgumentException =>
        Program.Tally(e.Message, ")BZ" + want + R"BZ(");
      end;
    end for;
    Console.WriteLine($"caught={K.caught} other={K.other} oda={K.oda} odt={K.odt} fin={K.fin} saw={K.sawBeforeFinally} cells={Cell.n}");
  end Main;
end Program;
)BZ";
}

void expectOut(const std::string& src, const char* want) {
    bezel::test::JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// The issue's shape: a direct `throw` after the binding. Static head.
BEZEL_TEST(await_using_exception_edge_awaits,
           direct_throw_static_head_awaits_dispose_async_spec_28_12_7705) {
    expectOut(build(R"BZ(
    await using var o := new NOK();
    throw new InvalidOperationException("boom");
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// A run-time head (an `owned IAsyncDisposable` call: the itab arm).
BEZEL_TEST(await_using_exception_edge_awaits,
           direct_throw_runtime_head_awaits_dispose_async_spec_28_12_7705) {
    expectOut(build(R"BZ(
    await using var o := Program.MkI();
    throw new InvalidOperationException("boom");
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// A DisposeAsync that completes inline (no genuine suspension).
BEZEL_TEST(await_using_exception_edge_awaits,
           direct_throw_inline_completing_dispose_async_spec_28_12_7705) {
    expectOut(build(R"BZ(
    await using var o := new NIN();
    throw new InvalidOperationException("boom");
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// The exception edge is a throwing CALL (a pending check), not a `throw`.
BEZEL_TEST(await_using_exception_edge_awaits,
           throwing_call_awaits_dispose_async_spec_28_12_7705) {
    expectOut(build(R"BZ(
    await using var o := new NOK();
    var k := Program.Boom();
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// The exception edge is an awaited task's fault re-raised (§30.9).
BEZEL_TEST(await_using_exception_edge_awaits,
           awaited_task_fault_awaits_dispose_async_spec_28_12_7705) {
    expectOut(build(R"BZ(
    await using var o := new NOK();
    await Program.BoomAsync();
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// Two bindings: both are awaited, once each, newest first.
BEZEL_TEST(await_using_exception_edge_awaits,
           two_heads_both_awaited_once_each_spec_28_12_7705) {
    expectOut(build(R"BZ(
    await using var a := new NOK();
    await using var b := Program.MkI();
    throw new InvalidOperationException("boom");
)BZ", "boom"),
              "caught=100 other=0 oda=200 odt=0 fin=0 saw=0 cells=0\n");
}

// A string local declared after the binding is released once (the #7630 shape),
// and a `using var` before it is disposed once.
BEZEL_TEST(await_using_exception_edge_awaits,
           string_and_using_var_locals_released_once_spec_28_12_7705) {
    expectOut(build(R"BZ(
    using var c := new Cell();
    await using var a := new NOK();
    var s := "x" + K.oda.ToString();
    var t := s + "y";
    throw new InvalidOperationException("boom");
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// The binding is inside a `try` of the same method: it is awaited BEFORE the
// finally runs (§28.10: cleanups established in the protected block run first).
BEZEL_TEST(await_using_exception_edge_awaits,
           try_finally_disposes_before_the_finally_spec_28_12_7705) {
    expectOut(build(R"BZ(
    try
      await using var o := new NOK();
      throw new InvalidOperationException("boom");
    finally
      K.fin := K.fin + 1;
      if K.oda = K.fin then K.sawBeforeFinally := K.sawBeforeFinally + 1; end;
    end;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=100 saw=100 cells=0\n");
}

// The binding is inside a `try` caught in the same method (issue #3121's shape).
BEZEL_TEST(await_using_exception_edge_awaits,
           caught_in_same_method_awaits_before_the_arm_spec_28_12_7705) {
    expectOut(build(R"BZ(
    try
      await using var o := new NOK();
      var k := Program.Boom();
    catch var e: InvalidOperationException =>
      do
        if K.oda = K.fin + 1 then K.sawBeforeFinally := K.sawBeforeFinally + 1; end;
        K.fin := K.fin + 1;
        throw new InvalidOperationException("boom");
      end;
    end;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=100 saw=100 cells=0\n");
}

// A throw out of a catch arm that holds a binding of its own.
BEZEL_TEST(await_using_exception_edge_awaits,
           binding_inside_a_catch_arm_awaits_on_rethrow_spec_28_12_7705) {
    expectOut(build(R"BZ(
    try
      throw new ArgumentException("first");
    catch var e: ArgumentException =>
      do
        await using var o := new NOK();
        throw new InvalidOperationException("boom");
      end;
    end;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// §28.4 / §31.2: DisposeAsync itself throws SYNCHRONOUSLY while the first
// exception unwinds: the new exception supersedes, the superseded one is
// disposed once (LeakSanitizer-clean), the owned Cell is cascaded.
BEZEL_TEST(await_using_exception_edge_awaits,
           disposeasync_sync_throw_supersedes_in_flight_exception_spec_31_2_7705) {
    expectOut(build(R"BZ(
    await using var o := new NSYNC();
    throw new InvalidOperationException("boom");
)BZ", "sboom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// Same, DisposeAsync completes by FAULTING its task.
BEZEL_TEST(await_using_exception_edge_awaits,
           disposeasync_faulted_task_supersedes_in_flight_exception_spec_31_2_7705) {
    expectOut(build(R"BZ(
    await using var o := new NFAULT();
    throw new InvalidOperationException("boom");
)BZ", "fboom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// §28.4: a faulting disposal does not skip the remaining ones. The newer
// binding faults; the older one is still awaited; the fault propagates.
BEZEL_TEST(await_using_exception_edge_awaits,
           faulting_disposal_does_not_skip_the_older_binding_spec_28_4_7705) {
    expectOut(build(R"BZ(
    await using var older := new NOK();
    await using var newer := new NFAULT();
    throw new InvalidOperationException("boom");
)BZ", "fboom"),
              "caught=100 other=0 oda=200 odt=0 fin=0 saw=0 cells=0\n");
}

// An async lambda body is an async body too.
BEZEL_TEST(await_using_exception_edge_awaits,
           async_lambda_body_awaits_on_the_exception_edge_spec_28_12_7705) {
    expectOut(build(R"BZ(
    var f := async lambda(): Task => do
      await using var o := new NOK();
      throw new InvalidOperationException("boom");
    end;
    await f();
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// ---------------------------------------------------------------------------
// A BOUND faulted task's exception across a DisposeAsync that REALLY suspends.
// `await t` of a bound task re-raises the task's own error object, BORROWED
// (§30.4: a second `await t` must find it live; §30.9: the task owns its fault
// and disposes it at its own scope end, deferring while the fault is in
// flight). The runtime's borrowed / deferred marks WERE per-thread sets, and the
// pass resumes from `Task.Delay(1)` on another worker: t's scope-end there saw
// the fault as NOT in flight and freed it, and the catch (or the caller) read
// the freed object (heap-use-after-free under ASan, MEASURED on the rejected
// 7705 tips). The marks are facts about the instance and are now process-wide.

// The minimal shape: t's scope ends after the binding's awaited disposal.
BEZEL_TEST(await_using_exception_edge_awaits,
           bound_task_fault_survives_suspending_dispose_async_spec_30_9_7705) {
    expectOut(build(R"BZ(
    var t := Program.BoomLater();
    await using var o := new NDLY();
    await t;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// DisposeAsync faults after suspending: its exception supersedes the borrowed
// fault, which stays the task's to dispose (§28.4 / §30.9).
BEZEL_TEST(await_using_exception_edge_awaits,
           bound_task_fault_superseded_by_faulting_dispose_async_spec_30_9_7705) {
    expectOut(build(R"BZ(
    var t := Program.BoomLater();
    await using var o := new NDFAULT();
    await t;
)BZ", "fboom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// The fault is caught in the same method and t is awaited AGAIN (§30.4: the
// same live instance is re-raised).
BEZEL_TEST(await_using_exception_edge_awaits,
           bound_task_awaited_again_after_suspending_dispose_spec_30_4_7705) {
    expectOut(build(R"BZ(
    var t := Program.BoomLater();
    try
      await using var o := new NDLY();
      await t;
    catch var e: InvalidOperationException =>
      K.fin := K.fin + 1;
    end;
    await t;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=100 saw=0 cells=0\n");
}

// t is bound INSIDE the try, so its scope-end runs on the unwind pass with its
// own fault in flight: the deferral (§30.9) must survive the suspend and the
// catch arm still reads the exception.
BEZEL_TEST(await_using_exception_edge_awaits,
           task_bound_inside_try_fault_read_by_the_arm_spec_30_9_7705) {
    expectOut(build(R"BZ(
    try
      var t := Program.BoomLater();
      await using var o := new NDLY();
      await t;
    catch var e: InvalidOperationException =>
      do
        if e.Message = "boom" then K.fin := K.fin + 1; end;
      end;
    end;
    throw new InvalidOperationException("boom");
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=100 saw=0 cells=0\n");
}

// t outlives the try; the arm handles the borrowed fault, then a fresh throw.
BEZEL_TEST(await_using_exception_edge_awaits,
           bound_task_fault_handled_after_suspending_dispose_spec_30_9_7705) {
    expectOut(build(R"BZ(
    var t := Program.BoomLater();
    try
      await using var o := new NDLY();
      await t;
    catch var e: InvalidOperationException =>
      K.fin := K.fin + 1;
    end;
    throw new InvalidOperationException("boom");
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=100 saw=0 cells=0\n");
}

// The supersede tail must NOT dispose a borrowed fault the task still owns:
// DisposeAsync throws synchronously, the arm catches its exception, and t is
// awaited again.
BEZEL_TEST(await_using_exception_edge_awaits,
           supersede_keeps_borrowed_fault_sync_throw_then_reawait_spec_30_4_7705) {
    expectOut(build(R"BZ(
    var t := Program.BoomLater();
    try
      await using var o := new NSYNC();
      await t;
    catch var e: ArgumentException =>
      K.fin := K.fin + 1;
    end;
    await t;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=100 saw=0 cells=0\n");
}

// Same, DisposeAsync faults after suspending.
BEZEL_TEST(await_using_exception_edge_awaits,
           supersede_keeps_borrowed_fault_async_fault_then_reawait_spec_30_4_7705) {
    expectOut(build(R"BZ(
    var t := Program.BoomLater();
    try
      await using var o := new NDFAULT();
      await t;
    catch var e: ArgumentException =>
      K.fin := K.fin + 1;
    end;
    await t;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=100 saw=0 cells=0\n");
}

// Every throw site that unwinds through a head shares ONE awaited-disposal
// block for it, and each site resumes its OWN pass afterwards: here a site
// inside a nested block (which must still dispose its `using var` Cell), a
// throwing call and a direct throw, through two heads. Per 3 iterations: the
// first site disposes `a` only, the other two dispose `b` then `a`.
BEZEL_TEST(await_using_exception_edge_awaits,
           throw_sites_sharing_a_head_each_resume_their_own_pass_spec_28_12_7705) {
    expectOut(build(R"BZ(
    K.fin := K.fin + 1;
    await using var a := new NDLY();
    var s := "x" + K.fin.ToString();
    if K.fin mod 3 = 0 then
      using var c := new Cell();
      throw new InvalidOperationException("boom");
    end;
    await using var b := new NDLY();
    if K.fin mod 3 = 1 then
      var k := Program.Boom();
    end;
    throw new InvalidOperationException("boom");
)BZ", "boom"),
              "caught=100 other=0 oda=167 odt=0 fin=100 saw=0 cells=0\n");
}

// ---------------------------------------------------------------------------
// The head itself OWNS the faulted task (`owned t: Task` field): the in-flight
// exception is t's fault, borrowed, and the head's §28.12 step 2 reclamation
// (the owned-field cascade) is what meets the §30.9 "task disposed while its
// fault is in flight" rule — it must DEFER the fault's disposal to the end of
// handling, not free it. The second rejected 7705 attempt parked the marks
// (erased them from the thread's sets) across the await and restored them only
// after that reclamation, which therefore saw the fault as owned and freed it
// mid-flight (heap-use-after-free, MEASURED: review r37-rev-u-await2 r04, r04i,
// r06). The marks are never moved now.

// DisposeAsync really suspends (`Task.Delay(1)`).
BEZEL_TEST(await_using_exception_edge_awaits,
           head_owning_the_faulted_task_suspending_dispose_async_spec_30_9_7705) {
    expectOut(build(R"BZ(
    await using var o := new NTDLY(Program.BoomLater());
    await o.t;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// DisposeAsync completes inline.
BEZEL_TEST(await_using_exception_edge_awaits,
           head_owning_the_faulted_task_inline_dispose_async_spec_30_9_7705) {
    expectOut(build(R"BZ(
    await using var o := new NTIN(Program.BoomLater());
    await o.t;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// The fault is caught in the same method: the arm reads the deferred instance
// after the suspending disposal, and the arm's exit disposes it (§30.9).
BEZEL_TEST(await_using_exception_edge_awaits,
           head_owning_the_faulted_task_caught_in_the_same_method_spec_30_9_7705) {
    expectOut(build(R"BZ(
    try
      await using var o := new NTDLY(Program.BoomLater());
      await o.t;
    catch var e: InvalidOperationException =>
      do
        if e.Message = "boom" then K.fin := K.fin + 1; end;
      end;
    end;
)BZ", "boom"),
              "caught=0 other=0 oda=100 odt=0 fin=100 saw=0 cells=0\n");
}

// DisposeAsync awaits the owned faulted task AGAIN: the fault edge re-raises
// the SAME instance (§30.4), which is not a supersede — it simply stays in
// flight.
BEZEL_TEST(await_using_exception_edge_awaits,
           head_owning_the_faulted_task_reawaited_by_dispose_async_spec_30_4_7705) {
    expectOut(build(R"BZ(
    await using var o := new NTRE(Program.BoomLater());
    await o.t;
)BZ", "boom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// DisposeAsync faults with a NEW exception after the owned task was reclaimed
// (its fault deferred): the supersede tail drains the deferred instance once
// (§31.2 / §30.9).
BEZEL_TEST(await_using_exception_edge_awaits,
           head_owning_the_faulted_task_faulting_dispose_async_drains_once_spec_31_2_7705) {
    expectOut(build(R"BZ(
    await using var o := new NTF(Program.BoomLater());
    await o.t;
)BZ", "fboom"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}

// ---------------------------------------------------------------------------
// A BORROWED caught exception that is NOT the one in flight: the arm holds a
// bound task's fault and throws a fresh exception through a head. The marks
// of the caught instance must still be found by t's scope-end after the
// suspending disposal, on whatever worker the pass resumes (MEASURED on the
// rejected tip: review r37-rev-u-await2 r15, r33 — t's scope-end on the resumed
// worker freed the instance the supersede had already disposed).

// The head is bound inside the arm.
BEZEL_TEST(await_using_exception_edge_awaits,
           borrowed_caught_fault_arm_head_throws_through_it_spec_30_9_7705) {
    expectOut(build(R"BZ(
    var t := Program.BoomLater();
    try
      await t;
    catch var e: InvalidOperationException =>
      do
        await using var o := new NDLY();
        K.fin := K.fin + 1;
        throw new ArgumentException("inner");
      end;
    end;
)BZ", "inner"),
              "caught=100 other=0 oda=100 odt=0 fin=100 saw=0 cells=0\n");
}

// The head is OUTSIDE the try; the arm's throw unwinds through it.
BEZEL_TEST(await_using_exception_edge_awaits,
           borrowed_caught_fault_arm_throw_unwinds_outer_head_spec_30_9_7705) {
    expectOut(build(R"BZ(
    var t := Program.BoomLater();
    await using var o := new NDLY();
    try
      await t;
    catch var e: InvalidOperationException =>
      do
        if e.Message.Length = 4 then throw new ArgumentException("inner"); end;
      end;
    end;
)BZ", "inner"),
              "caught=100 other=0 oda=100 odt=0 fin=0 saw=0 cells=0\n");
}
