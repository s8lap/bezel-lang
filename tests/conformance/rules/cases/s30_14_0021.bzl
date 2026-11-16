//! case: s30_14_0021
//! doc: spec
//! section: 30.14
//! title: a child domain ending in OperationCanceledException joins as DomainFaultedException
//! rule: "Every other exception joins as `DomainFaultedException` itself, cancellation included: an `OperationCanceledException` that propagates out of the entry does not cross the join as cancellation, so an arm naming `OperationCanceledException` does not take the join's exception, and an arm naming `DomainFaultedException` does."
//! cites: 30.14, 30.8, 31.4
//! expect: run
//! stdout: joined as a fault
//! issue: 8418
module Conf;

import System.Threading;

public class Program
  public static method Stop(): int
    throw new OperationCanceledException("stop");
  end Stop;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Stop);
    try
      try
        var n := await t;
      catch
        OperationCanceledException => Console.WriteLine("taken as cancellation")
      end;
    catch
      DomainFaultedException => Console.WriteLine("joined as a fault")
    end;
  end Main;
end Program;
