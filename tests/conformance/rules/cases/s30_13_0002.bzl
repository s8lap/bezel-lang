//! case: s30_13_0002
//! doc: spec
//! section: 30.13
//! title: an await of an already-completed task does not suspend the synchronous start of an async call
//! rule: Calling an async method allocates the state machine and begins
//!   execution synchronously, as part of the running continuation, until the
//!   first `await` of an incomplete task.
//! cites: 30.13, 30.5
//! expect: run
//! output: A
//!   A5
//!   B
//!   C1
//! notes: the first await is of Task.FromResult's completed task, so execution
//!   continues synchronously past it; only the await of the incomplete gate
//!   returns control to Main. Twin of s30_13_0001.
module Conf;

import System.Threading;

public class Program
  public static async method Work(gate: Task<int>): Task
    Console.WriteLine("A");
    var k := await Task.FromResult(5);
    Console.WriteLine($"A{k}");
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
