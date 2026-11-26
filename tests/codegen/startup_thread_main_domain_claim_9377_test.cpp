// Codegen + runtime — #9377 (and #9195): the startup thread holds the main
// domain's claim outside its process-edge waits (§30.13, v0.360 ruling B).
//
// THE RULE (owner ruling on #9377, queued as a §30.13 clarification for
// v0.360). While the startup thread runs program code — a non-`async` Main
// and what it calls directly, and an `async` Main's synchronous prefix — it
// holds the main domain's claim, so no main-domain continuation runs at the
// same time. It releases the claim only while it is blocked at a process-edge
// wait (§30.3's `Wait()` / `Result`, §30.12's blocking acquisitions, and the
// runtime's implicit scope-end borrow wait of #4540), takes it back before
// returning to program code, and releases it for good when Main returns.
//
// WHAT WAS WRONG. The startup thread ran unserialised against the main
// domain's workers: a continuation of an async method it started could run
// in parallel with the code after the call. In
// async_frame_teardown_no_uaf_3460_test.cpp's kN24 program that is the
// `bz_closure_release` double free the 3460 row reports under parallel load
// (bisected to fec738642, #9096, which only widened the window), and in
// #9195's program it is lost updates between an async Main's prefix and a
// continuation it woke.
//
// ORACLES: runAot / runAotChecked only, `r.ran` asserted directly.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

// =====================================================================
// §30.13 (v0.360 ruling B on #9377): the 3460 row's kN24 shape, looped 200
// times in one process with a widening busy window between starting `Fill`
// and calling `c.Call()`. `Fill`'s continuation (the `this._h := ...` store
// after `await Task.Delay(0)`) is main-domain code, so it cannot run until the
// startup thread blocks — here, at `c`'s scope-end borrow wait — and every
// `c.Call()` returns the initial `_h`, 0. The ruling states this program shape
// is defined and prints `h=0`.
//
// Fail-without (measured on r30-c1 da39776e2, 10 of 10 runs): the
// continuation overtakes the startup thread in most iterations, `sum=1057`
// to `sum=1141`. The checked oracle also carries the ASan leg of the double
// free the two overlapping threads can produce.
// =====================================================================
BEZEL_TEST(startup_thread_main_domain_claim_9377,
           startup_code_runs_before_the_continuation_it_started_spec_30_13) {
    JitResult r = runAotChecked(R"BZ(
module M; import System; import System.Threading.Tasks;
class C
  private _h: () -> int := lambda() => 0;
  public async method Fill(u: int): Task<int>
    var k: int := u;
    var e := lambda() => k;
    var s: int := e();
    await Task.Delay(0);
    this._h := lambda() => k;
    return s;
  end Fill;
  public method Call(): int
    return this._h();
  end Call;
end C;
class D
  public static method Main(): int
    var sum: int := 0;
    for var i in 1..200 do
      using var c := new C();
      var t := c.Fill(7);
      var w: int := 0;
      for var j in 1..i * 100 do w := w + j; end;
      sum := sum + c.Call();
    end;
    Console.WriteLine("sum=" + sum.ToString());
    return 0;
  end Main;
end D;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("sum=0\n"));
}

// =====================================================================
// §30.13 (v0.359: "An `async` `Main` is driven as the main domain's first
// continuation"; v0.360 ruling B on #9377) — #9195's own repro. Main's
// synchronous prefix wakes P's parked `SendAsync` (the `TryReceive`) and then
// runs its own loop; P's continuation is main-domain code too, so the two
// loops must not overlap and every increment of the shared static survives.
//
// Fail-without (measured on r30-c1 da39776e2, 10 of 10 runs): `count 2001048`
// to `count 2360226`.
// =====================================================================
BEZEL_TEST(startup_thread_main_domain_claim_9377,
           async_main_sync_prefix_does_not_overlap_a_woken_continuation_9195_spec_30_13) {
    JitResult r = runAot(R"BZ(
module P;
import System.Threading;
public static class C
  public static count: int := 0;
  public static async method P(owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    await sender.SendAsync(1);
    await sender.SendAsync(2);
    for var i in 1..2000000 do C.count := C.count + 1; end;
  end P;
end C;
public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>(1);
    var s := ch.NewSender();
    using var p := C.P(move s);
    var item: int? := null;
    if not ch.TryReceive(out item) then Console.WriteLine("no item"); end;
    for var i in 1..2000000 do C.count := C.count + 1; end;
    await p;
    Console.WriteLine($"count {C.count}");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("count 4000000\n"));
}

// =====================================================================
// CONTROL 1 of 2 (green on the base; red — a hang, caught by the harness's
// run deadline — under the plausible wrong fix that takes the claim at Main's
// entry but does NOT give it up inside `Wait()` / `Result`). §30.3: a
// startup-thread `Wait()` / `Result` must make progress, so it releases the
// main domain's claim while it blocks. Covers a direct `Wait()`, a `Result`
// reached through a helper on a task whose continuations await further
// main-domain tasks, two tasks waited in sequence, and a task still pending
// when Main returns.
// =====================================================================
BEZEL_TEST(startup_thread_main_domain_claim_9377,
           control_startup_wait_and_result_release_the_claim_spec_30_3) {
    JitResult r = runAotChecked(R"BZ(
module M; import System; import System.Threading.Tasks;
class C
  public static async method Twice(u: int): Task<int>
    await Task.Delay(1);
    return u * 2;
  end Twice;
  public static async method Chain(u: int): Task<int>
    var a := await C.Twice(u);
    await Task.Delay(1);
    var b := await C.Twice(a);
    return b;
  end Chain;
  public static method Helper(u: int): int
    using var t := C.Chain(u);
    return t.Result;
  end Helper;
end C;
class D
  public static method Main(): int
    using var t := C.Twice(5);
    t.Wait();
    Console.WriteLine("wait=" + t.Result.ToString());
    var r := C.Helper(3);
    Console.WriteLine("nested=" + r.ToString());
    using var x := C.Chain(1);
    using var y := C.Chain(2);
    Console.WriteLine("two=" + (x.Result + y.Result).ToString());
    using var late := C.Twice(100);
    return 0;
  end Main;
end D;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("wait=10\nnested=12\ntwo=12\n"));
}

// =====================================================================
// CONTROL 2 of 2 (green on the base; red — a hang — under the plausible wrong
// fix that does NOT give the claim up inside a blocking acquisition). §30.12:
// a blocking `Semaphore.Acquire()` / `Mutex.Acquire()` follows §30.3's
// discipline, so on the startup thread it releases the main domain's claim
// while it is parked. `Hold` takes both locks asynchronously and keeps them
// across an await; its continuation (main-domain code) must run for them to
// be released, which it can only do while `Take` is parked.
// =====================================================================
BEZEL_TEST(startup_thread_main_domain_claim_9377,
           control_startup_blocking_acquisition_releases_the_claim_spec_30_12) {
    JitResult r = runAotChecked(R"BZ(
module M; import System; import System.Threading; import System.Threading.Tasks;
class C
  public static async method Hold(m: Mutex, sem: Semaphore): Task<int>
    using var g := await m.AcquireAsync();
    using var tok := await sem.AcquireAsync();
    await Task.Delay(20);
    return 1;
  end Hold;
end C;
class D
  public static method Take(m: Mutex, sem: Semaphore): int
    using var tok := sem.Acquire();
    using var g := m.Acquire();
    return 1;
  end Take;
  public static method Main(): int
    using var m := new Mutex();
    using var sem := new Semaphore(1, 1);
    using var t := C.Hold(m, sem);
    using var d := Task.Delay(5);
    d.Wait();
    var a := D.Take(m, sem);
    Console.WriteLine("took=" + a.ToString() + " held=" + t.Result.ToString());
    return 0;
  end Main;
end D;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("took=1 held=1\n"));
}
