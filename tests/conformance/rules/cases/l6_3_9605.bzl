//! case: l6_3_9605
//! doc: stdlib
//! section: 6.3
//! title: cancelled async acquisitions never hold a permit
//! rule: fault with `OperationCanceledException`, a cancelled acquisition never
//!   holds the permit, null-on-timeout in the `Try` forms
//! cites: 6.3, 6.2
//! expect: run
//! stdout: total 900 free1 150
//! issue: 9055
module Conf;

import System.Threading;

public static class Jobs
  public static got: int := 0;
  public static cancelled: int := 0;

  public static async method Wait(gate: Semaphore, ms: int): Task
    using var cts := CancellationTokenSource.WithCancelAfter(TimeSpan.FromMilliseconds(ms));
    try
      using var permit := await gate.AcquireAsync(cts.Token);
      await Task.Delay(1);
      Jobs.got := Jobs.got + 1;
    catch OperationCanceledException => Jobs.cancelled := Jobs.cancelled + 1;
    end try;
  end Wait;
end Jobs;

public class Program
  public static async method Main(): Task
    await Task.Delay(1);
    var free1 := 0;
    for var round in 1..150 do
      using var gate := new Semaphore(1);
      using var jobs := new OwningArray<Task>();
      for var i in 1..6 do
        jobs.Add(Jobs.Wait(gate, i mod 3 + 1));
      end for;
      for var j in jobs do
        await j;
      end for;
      using var a := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      using var b := await gate.TryAcquireAsync(TimeSpan.FromMilliseconds(0));
      if a <> null and b = null then
        free1 += 1;
      end if;
    end for;
    var total := Jobs.got + Jobs.cancelled;
    Console.WriteLine($"total {total} free1 {free1}");
  end Main;
end Program;
