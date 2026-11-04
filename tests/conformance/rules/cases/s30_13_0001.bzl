//! case: s30_13_0001
//! doc: spec
//! section: 30.13
//! title: calling an async method runs its body synchronously up to the first await of an incomplete task
//! rule: Calling an async method allocates the state machine and begins
//!   execution synchronously, as part of the running continuation, until the
//!   first `await` of an incomplete task.
//! cites: 30.13, 30.11
//! expect: run
//! output: A
//!   B
//!   C1
module Conf;

import System.Threading;

public class Program
  public static async method Work(gate: Task<int>): Task
    Console.WriteLine("A");
    var n := await gate;
    Console.WriteLine($"C{n}");
  end Work;

  public static async method Main(): Task
    using var tcs := new TaskCompletionSource<int>();
    using var t := Work(tcs.Task);
    Console.WriteLine("B");
    tcs.SetResult(1);
    await t;
  end Main;
end Program;
