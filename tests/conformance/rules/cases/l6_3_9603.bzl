//! case: l6_3_9603
//! doc: stdlib
//! section: 6.3
//! title: blocking, timed and async acquisitions of one permit all complete
//! rule: An acquisition attempted while no permit remains waits —
//!   `Acquire`/`AcquireAsync` until one is returned,
//!   `TryAcquire`/`TryAcquireAsync` until then or until the timeout, whichever
//!   comes first
//! cites: 6.3, 6.2
//! expect: run
//! stdout: mine 100 timed 20 async 100
//! issue: 9055
module Conf;

import System.Threading;

public static class Jobs
  public static async method Run(gate: Semaphore): Task<int>
    var n := 0;
    for var k in 1..20 do
      await Task.Delay(1);
      using var permit := await gate.AcquireAsync();
      await Task.Delay(1);
      n += 1;
    end for;
    return n;
  end Run;
end Jobs;

public class Program
  public static method Main()
    using var gate := new Semaphore(1);
    using var t1 := Jobs.Run(gate);
    using var t2 := Jobs.Run(gate);
    using var t3 := Jobs.Run(gate);
    using var t4 := Jobs.Run(gate);
    using var t5 := Jobs.Run(gate);
    var mine := 0;
    for var k in 1..100 do
      using var p := gate.Acquire();
      mine += 1;
    end for;
    var timedOk := 0;
    for var k in 1..20 do
      using var q := gate.TryAcquire(TimeSpan.FromSeconds(10));
      if q <> null then
        timedOk += 1;
      end if;
    end for;
    t1.Wait();
    t2.Wait();
    t3.Wait();
    t4.Wait();
    t5.Wait();
    var sum := t1.Result + t2.Result + t3.Result + t4.Result + t5.Result;
    Console.WriteLine($"mine {mine} timed {timedOk} async {sum}");
  end Main;
end Program;
