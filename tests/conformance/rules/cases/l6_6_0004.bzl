//! case: l6_6_0004
//! doc: stdlib
//! section: 6.6
//! title: reading `Task` off a source after its dominating Dispose is a compile error
//! rule: destructor; // disposes the produced task, faulting it first if
//!   the source is disposed before completion. — a witness that reads
//!   `tcs.Task` AFTER a straight-line `tcs.Dispose()` never reaches that
//!   rule: "A use of a local variable — a read, a borrow taken from it, a
//!   member access ... — after an explicit `Dispose()` ... on the same
//!   variable that dominates the use site on every path is a **compile
//!   error**" (spec §28.2, v0.357; proposal 0205's corpus table).
//! cites: 6.6, 30.11, 28.2
//! expect: reject
//! diagnostic: use-after-dispose
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
