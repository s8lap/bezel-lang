//! case: l6_3_9602
//! doc: stdlib
//! section: 6.3
//! title: many async waiters on a three-permit Semaphore all complete and return every permit
//! rule: Each successful acquisition consumes one and yields the `SemaphoreToken`
//!   that holds it; disposing the token returns it.
//! cites: 6.3
//! expect: run
//! stdout: outs 1000 within True free3 5
//! issue: 9055
module Conf;

import System.Threading;

public static class Jobs
  public static inside: int := 0;
  public static maxInside: int := 0;
  public static outs: int := 0;

  public static async method Run(gate: Semaphore): Task
    using var permit := await gate.AcquireAsync();
    Jobs.inside := Jobs.inside + 1;
    if Jobs.inside > Jobs.maxInside then
      Jobs.maxInside := Jobs.inside;
    end if;
    await Task.Delay(1);
    Jobs.inside := Jobs.inside - 1;
    Jobs.outs := Jobs.outs + 1;
  end Run;
end Jobs;

public class Program
  public static async method Main(): Task
    await Task.Delay(1);
    var free3 := 0;
    for var round in 1..5 do
      using var gate := new Semaphore(3);
      using var jobs := new OwningArray<Task>();
      for var i in 1..200 do
        jobs.Add(Jobs.Run(gate));
      end for;
      for var j in jobs do
        await j;
      end for;
      using var a := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      using var b := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      using var c := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      using var d := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      if a <> null and b <> null and c <> null and d = null then
        free3 += 1;
      end if;
    end for;
    var within := Jobs.maxInside <= 3;
    Console.WriteLine($"outs {Jobs.outs} within {within} free3 {free3}");
  end Main;
end Program;
