//! case: s30_14_0018
//! doc: spec
//! section: 30.14
//! title: an unmarked exception raised in a child passes an IFailure arm at the join
//! rule: where the original exception's class implements `IFailure` (§31.4), the
//!   join raises `DomainFailedException`, which derives from
//!   `DomainFaultedException` and implements `IFailure` (stdlib §6.8). An arm
//!   naming `DomainFaultedException` takes both; an arm naming `IFailure` takes
//!   a failure raised in another domain as it takes one raised in its own.
//! cites: 30.14, 31.4
//! expect: run
//! stdout: unmarked joined
//! issue: 8418
module Conf;

import System.Threading;

public class Program
  public static method Explode(): int
    throw new InvalidOperationException("bad state");
  end Explode;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Explode);
    try
      try
        var n := await t;
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      DomainFaultedException => Console.WriteLine("unmarked joined")
    end;
  end Main;
end Program;
