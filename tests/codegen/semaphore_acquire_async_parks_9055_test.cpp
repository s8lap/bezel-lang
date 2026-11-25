// Codegen / runtime — stdlib §6.3 v0.359 (#9055): four or more tasks waiting
// in `Semaphore.AcquireAsync` stall the whole program, timers included.
//
// THE DEFECT (src/runtime/bezel_runtime_sync.cpp, bz_semaphore_acquire_async
// and bz_semaphore_try_acquire_async): a contended async acquisition was
// served by submitting a job to the scheduler's pool that BLOCKED a pool
// worker in sem_wait (sem_timedwait for TryAcquireAsync). The pool has four
// workers, and every domain continuation runs on one of them, so four
// waiters occupied the whole pool: the permit holder's continuation (after
// its `await Task.Delay(10)`) never ran, never disposed its token, and the
// program hung after `job 1 in`. With three waiters it completed.
//
// THE CONTRACT (stdlib §6.3 "Permits", v0.359):
//   "Each successful acquisition consumes one and yields the
//    `SemaphoreToken` that holds it; disposing the token returns it. An
//    acquisition attempted while no permit remains waits —
//    `Acquire`/`AcquireAsync` until one is returned,
//    `TryAcquire`/`TryAcquireAsync` until then or until the timeout,
//    whichever comes first."
// So every waiter is eventually granted a permit once the holders dispose
// theirs, however many wait, and no more than `initialCount` hold at once.
// stdlib §6.3 specifies no order among waiters, so no row asserts one: the
// literal-repro row counts each line rather than comparing a transcript.
//
// THE FIX: a contended async acquisition parks a waiter record (carrying its
// pending task) in the semaphore's FIFO; disposing a token hands the permit to
// the oldest waiter and completes its task, which resumes the awaiter in its
// own domain. No thread waits. TryAcquireAsync's timeout is a timer entry
// that withdraws a still-waiting record and completes it with null.
//
// DETERMINISM: each row runs many rounds inside ONE program and prints
// counts; a hang is caught by the harness's run deadline (a timeout fails
// the row, it does not hang the suite). Each `Main` first suspends on
// `await Task.Delay(1)`, so every round runs as a main-domain continuation
// (the startup prefix runs in parallel with the continuations it wakes,
// #9195), and the rows' static counters are touched by one domain only.
//
// ORACLES: runAot / runAotChecked only. No `wasSkipped` absorbing guard:
// `r.ran` is asserted directly.

#include <sstream>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int countLine(const std::string& text, const std::string& line) {
    std::istringstream in(text);
    std::string l;
    int n = 0;
    while (std::getline(in, l))
        if (l == line) ++n;
    return n;
}
int countLines(const std::string& text) {
    std::istringstream in(text);
    std::string l;
    int n = 0;
    while (std::getline(in, l)) ++n;
    return n;
}
}  // namespace

// =====================================================================
// stdlib §6.3 v0.359: the issue's repro (Semaphore(1), five jobs, one holds
// and four wait in AcquireAsync), run 20 times in one program. Every job
// must enter and leave in every round: each `job k in` / `job k out` line 20
// times, `one awaited` 100 times. Fail-without: the program prints `job 1 in`
// and `started` and hangs (run deadline).
// =====================================================================
BEZEL_TEST(semaphore_acquire_async_parks_9055,
           issue_repro_four_waiters_on_one_permit_all_enter_and_leave_stdlib_6_3) {
    JitResult r = runAot(R"BZ(
module P;
import System.Threading;
public static class Jobs
  public static async method Run(id: int, gate: Semaphore): Task
    using var permit := await gate.AcquireAsync();
    Console.WriteLine($"job {id} in");
    await Task.Delay(10);
    Console.WriteLine($"job {id} out");
  end Run;
  public static async method Round(): Task
    using var gate := new Semaphore(1);
    using var jobs := new OwningArray<Task>();
    for var i in 1..5 do
      jobs.Add(Jobs.Run(i, gate));
    end;
    Console.WriteLine("started");
    for var j in jobs do
      await j;
      Console.WriteLine("one awaited");
    end;
  end Round;
end Jobs;
public class Program
  public static async method Main(): Task
    await Task.Delay(1);  // leave the startup prefix first (#9195)
    for var round in 1..20 do
      await Jobs.Round();
    end;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    for (int k = 1; k <= 5; ++k) {
        BEZEL_EXPECT_EQ(countLine(r.stdoutText, "job " + std::to_string(k) + " in"), 20);
        BEZEL_EXPECT_EQ(countLine(r.stdoutText, "job " + std::to_string(k) + " out"), 20);
    }
    BEZEL_EXPECT_EQ(countLine(r.stdoutText, "started"), 20);
    BEZEL_EXPECT_EQ(countLine(r.stdoutText, "one awaited"), 100);
    BEZEL_EXPECT_EQ(countLines(r.stdoutText), 320);
}

// =====================================================================
// stdlib §6.3 v0.359, under the checked oracle: Semaphore(1) and
// Semaphore(2), each with 5 and with 16 tasks waiting in AcquireAsync
// behind the holders, 20 rounds per combination. Every round, every job
// must leave (count and id sum), at most `permits` may hold at once, and
// none may still be inside. Fail-without: hangs at the first round
// (run deadline).
// =====================================================================
BEZEL_TEST(semaphore_acquire_async_parks_9055,
           five_and_sixteen_waiters_on_one_and_two_permits_all_granted_stdlib_6_3) {
    JitResult r = runAotChecked(R"BZ(
module P;
import System.Threading;
public static class Jobs
  public static inside: int := 0;
  public static maxInside: int := 0;
  public static outs: int := 0;
  public static idSum: int := 0;
  public static async method Run(id: int, gate: Semaphore): Task
    using var permit := await gate.AcquireAsync();
    Jobs.inside := Jobs.inside + 1;
    if Jobs.inside > Jobs.maxInside then Jobs.maxInside := Jobs.inside; end;
    await Task.Delay(1);
    Jobs.inside := Jobs.inside - 1;
    Jobs.outs := Jobs.outs + 1;
    Jobs.idSum := Jobs.idSum + id;
  end Run;
  public static async method Round(permits: int, jobCount: int): Task<bool>
    Jobs.inside := 0;
    Jobs.maxInside := 0;
    Jobs.outs := 0;
    Jobs.idSum := 0;
    using var gate := new Semaphore(permits);
    using var jobs := new OwningArray<Task>();
    for var i in 1..jobCount do
      jobs.Add(Jobs.Run(i, gate));
    end;
    for var j in jobs do
      await j;
    end;
    return Jobs.outs = jobCount and Jobs.idSum = jobCount * (jobCount + 1) / 2
      and Jobs.maxInside <= permits and Jobs.inside = 0;
  end Round;
  public static method Waiters(wi: int): int
    if wi = 1 then return 5; end;
    return 16;
  end Waiters;
end Jobs;
public class Program
  public static async method Main(): Task
    await Task.Delay(1);  // leave the startup prefix first (#9195)
    for var permits in 1..2 do
      for var wi in 1..2 do
        var waiters := Jobs.Waiters(wi);
        var good := 0;
        for var round in 1..20 do
          if await Jobs.Round(permits, permits + waiters) then good += 1; end;
        end;
        Console.WriteLine($"permits {permits} waiters {waiters} good {good}");
      end;
    end;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("permits 1 waiters 5 good 20\n"
                                "permits 1 waiters 16 good 20\n"
                                "permits 2 waiters 5 good 20\n"
                                "permits 2 waiters 16 good 20\n"));
}

// =====================================================================
// stdlib §6.3 v0.359: TryAcquireAsync with a 20 s timeout waits "until
// [a permit is returned] or until the timeout, whichever comes first". Six
// tasks on Semaphore(1) (five waiting), each holding briefly, 20 rounds:
// every acquisition is granted long before its timeout (`got 120 missed 0`).
// Fail-without: five waiters blocked four pool workers in sem_timedwait, so
// the holder never released and the program ran into the run deadline.
// =====================================================================
BEZEL_TEST(semaphore_acquire_async_parks_9055,
           try_acquire_async_waiters_are_granted_before_their_timeout_stdlib_6_3) {
    JitResult r = runAotChecked(R"BZ(
module P;
import System.Threading;
public static class Jobs
  public static got: int := 0;
  public static missed: int := 0;
  public static async method Run(gate: Semaphore): Task
    using var permit := await gate.TryAcquireAsync(TimeSpan.FromSeconds(20));
    if permit <> null then
      await Task.Delay(1);
      Jobs.got := Jobs.got + 1;
    else
      Jobs.missed := Jobs.missed + 1;
    end;
  end Run;
end Jobs;
public class Program
  public static async method Main(): Task
    await Task.Delay(1);  // leave the startup prefix first (#9195)
    for var round in 1..20 do
      using var gate := new Semaphore(1);
      using var jobs := new OwningArray<Task>();
      for var i in 1..6 do
        jobs.Add(Jobs.Run(gate));
      end;
      for var j in jobs do
        await j;
      end;
    end;
    Console.WriteLine($"got {Jobs.got} missed {Jobs.missed}");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("got 120 missed 0\n"));
}

// =====================================================================
// Boundary control (green on the base too), stdlib §6.3 v0.359: a waiter
// whose TryAcquireAsync timeout passes first completes with null and never
// receives a permit. Three 2 ms TryAcquireAsync waiters behind a held permit
// all miss; the holder then disposes its token and an immediate
// TryAcquireAsync(0) must find the permit free, 50 rounds, under ASan/LSan.
// Red under the plausible wrong fix that leaves a timed-out waiter in the
// FIFO: the returned permit is handed to the dead waiter (measured with that
// mutant: the program hangs).
// =====================================================================
BEZEL_TEST(semaphore_acquire_async_parks_9055,
           timed_out_waiter_never_takes_the_returned_permit_stdlib_6_3) {
    JitResult r = runAotChecked(R"BZ(
module P;
import System.Threading;
public static class Jobs
  public static missed: int := 0;
  public static async method Try(gate: Semaphore): Task
    using var permit := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(2));
    if permit = null then Jobs.missed := Jobs.missed + 1; end;
  end Try;
end Jobs;
public class Program
  public static async method Main(): Task
    await Task.Delay(1);  // leave the startup prefix first (#9195)
    var after := 0;
    for var round in 1..50 do
      using var gate := new Semaphore(1);
      using var jobs := new OwningArray<Task>();
      var held := await gate.AcquireAsync();
      for var i in 1..3 do
        jobs.Add(Jobs.Try(gate));
      end;
      for var j in jobs do
        await j;
      end;
      held.Dispose();
      using var again := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      if again <> null then after += 1; end;
    end;
    Console.WriteLine($"missed {Jobs.missed} after {after}");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("missed 150 after 50\n"));
}

// =====================================================================
// Boundary control (green on the base too), stdlib §6.2 v0.359 (cancellation
// contract, adopted verbatim by §6.3): "Cancelling the token of a pending
// `AcquireAsync` ... faults the returned task with
// `OperationCanceledException`, and a cancelled acquisition never holds" the
// permit. Two tasks park behind a held permit; the first's token is
// cancelled, then the holder disposes its token: the first must fault
// (caught as OperationCanceledException) and the permit must reach the
// second, 50 rounds, under ASan/LSan. Red under the plausible wrong fix that
// hands the returned permit to the oldest waiter without checking its token
// (measured with that mutant: `cancelled 0 next 50`).
// =====================================================================
BEZEL_TEST(semaphore_acquire_async_parks_9055,
           cancelled_parked_waiter_faults_and_the_permit_passes_on_stdlib_6_3) {
    JitResult r = runAotChecked(R"BZ(
module P;
import System.Threading;
public static class Jobs
  public static async method Wait(gate: Semaphore, tok: CancellationToken): Task<int>
    try
      using var permit := await gate.AcquireAsync(tok);
      return 1;
    catch OperationCanceledException => return 2;
    end;
  end Wait;
end Jobs;
public class Program
  public static async method Main(): Task
    await Task.Delay(1);  // leave the startup prefix first (#9195)
    var cancelledSeen := 0;
    var nextGot := 0;
    for var round in 1..50 do
      using var gate := new Semaphore(1);
      using var cts := new CancellationTokenSource();
      var held := await gate.AcquireAsync();
      using var first := Jobs.Wait(gate, cts.Token);
      using var second := Jobs.Wait(gate, CancellationToken.None);
      await Task.Delay(1);
      cts.Cancel();
      held.Dispose();
      if await first = 2 then cancelledSeen += 1; end;
      if await second = 1 then nextGot += 1; end;
    end;
    Console.WriteLine($"cancelled {cancelledSeen} next {nextGot}");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("cancelled 50 next 50\n"));
}
