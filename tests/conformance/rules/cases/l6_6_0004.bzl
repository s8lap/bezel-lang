//! case: l6_6_0004
//! doc: stdlib
//! section: 6.6
//! title: disposing the source before completion faults the produced task
//! rule: destructor; // disposes the produced task, faulting it first if
//!   the source is disposed before completion.
//! cites: 6.6, 30.11
//! expect: run
//! stdout: source disposed before completion
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    try
      do
        var tcs := new TaskCompletionSource<int>();
        tcs.Dispose();
        var t := tcs.Task;
        await t;
      end;
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
