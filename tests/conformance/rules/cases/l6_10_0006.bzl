//! case: l6_10_0006
//! doc: stdlib
//! section: 6.10
//! title: Spawn after the scope has closed raises InvalidOperationException
//! rule: Spawn after close (after Join returned or disposal began) raises
//!   InvalidOperationException — deterministic, never a silent extension
//!   of the drain.
//! cites: 6.10
//! expect: run
//! stdout: rejected
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    await using var scope := new TaskScope();
    await scope.Join();
    try
      scope.Spawn(lambda() => Task.Delay(0));
    catch
      var ex: InvalidOperationException => Console.WriteLine("rejected"),
    end try;
  end Main;
end Program;
