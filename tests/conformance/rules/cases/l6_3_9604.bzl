//! case: l6_3_9604
//! doc: stdlib
//! section: 6.3
//! title: timed async acquisitions racing a returned permit account for every job and return every permit
//! rule: An acquisition attempted while no permit remains waits —
//!   `Acquire`/`AcquireAsync` until one is returned,
//!   `TryAcquire`/`TryAcquireAsync` until then or until the timeout, whichever
//!   comes first
//! cites: 6.3
//! expect: run
//! stdout: total 1200 free2 150
//! issue: 9055
module Conf;

import System.Threading;

public static class Jobs
  public static got: int := 0;
  public static missed: int := 0;

  public static async method Run(gate: Semaphore, ms: int): Task
    using var permit := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(ms));
    if permit <> null then
      await Task.Delay(1);
      Jobs.got := Jobs.got + 1;
    else
      Jobs.missed := Jobs.missed + 1;
    end if;
  end Run;
end Jobs;

public class Program
  public static async method Main(): Task
    await Task.Delay(1);
    var free2 := 0;
    for var round in 1..150 do
      using var gate := new Semaphore(2);
      using var jobs := new OwningArray<Task>();
      for var i in 1..8 do
        jobs.Add(Jobs.Run(gate, i mod 3 + 1));
      end for;
      for var j in jobs do
        await j;
      end for;
      using var a := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      using var b := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      using var c := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      if a <> null and b <> null and c = null then
        free2 += 1;
      end if;
    end for;
    var total := Jobs.got + Jobs.missed;
    Console.WriteLine($"total {total} free2 {free2}");
  end Main;
end Program;
