//! case: l6_3_9601
//! doc: stdlib
//! section: 6.3
//! title: a Semaphore with no permits times every timed async acquisition out
//! rule: An acquisition attempted while no permit remains waits —
//!   `Acquire`/`AcquireAsync` until one is returned,
//!   `TryAcquire`/`TryAcquireAsync` until then or until the timeout, whichever
//!   comes first
//! cites: 6.3
//! expect: run
//! stdout: missed 300
//! issue: 9055
module Conf;

import System.Threading;

public static class Jobs
  public static missed: int := 0;

  public static async method Try(gate: Semaphore): Task
    using var permit := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(2));
    if permit = null then
      Jobs.missed := Jobs.missed + 1;
    end if;
  end Try;
end Jobs;

public class Program
  public static async method Main(): Task
    await Task.Delay(1);
    for var round in 1..50 do
      using var gate := new Semaphore(0);
      using var jobs := new OwningArray<Task>();
      for var i in 1..6 do
        jobs.Add(Jobs.Try(gate));
      end for;
      for var j in jobs do
        await j;
      end for;
    end for;
    Console.WriteLine($"missed {Jobs.missed}");
  end Main;
end Program;
