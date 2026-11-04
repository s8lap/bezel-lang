//! case: s30_13_0003
//! doc: spec
//! section: 30.13
//! title: completing an awaited task queues the continuation rather than running it inside the completer
//! rule: a continuation, once started, runs without interruption from the
//!   domain's other work until it suspends at an `await`, completes, or
//!   propagates an exception (*run-to-completion*). … When the awaited task completes, the continuation is queued to its
//!   domain. The domain runs queued continuations one at a time, in
//!   unspecified order.
//! cites: 30.13, 30.11
//! expect: run
//! output: A
//!   B
//!   D
//!   C1
//! notes: SetResult completes the gate from Main's running continuation; the
//!   awaiting continuation is queued and cannot run until Main suspends at
//!   `await t`, so D precedes C1 (run-to-completion, Execution domains).
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
    Console.WriteLine("D");
    await t;
  end Main;
end Program;
