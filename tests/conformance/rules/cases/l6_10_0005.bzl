//! case: l6_10_0005
//! doc: stdlib
//! section: 6.10
//! title: a child's fault re-raises at await Join()
//! rule: await Join(): Task; // await all children; re-raise the FIRST
//!   fault (first-fault-wins, no aggregation)
//! cites: 6.10
//! expect: run
//! stdout: boom
module Conf;

import System.Threading;

public class Program
  public static async method Boom(): Task
    await Task.Delay(0);
    throw new InvalidOperationException("boom");
  end Boom;

  public static async method Main(): Task
    await using var scope := new TaskScope();
    scope.Spawn(Program.Boom);
    try
      await scope.Join();
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
