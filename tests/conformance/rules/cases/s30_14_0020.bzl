//! case: s30_14_0020
//! doc: spec
//! section: 30.14
//! title: a marked subclass of InvalidOperationException raised in a child is joined as DomainFailedException
//! rule: where the original exception's class implements `IFailure` (§31.4),
//!   the join raises `DomainFailedException`, which derives from
//!   `DomainFaultedException` and implements `IFailure` (stdlib §6.8) … Every
//!   other `Exception`-derived class may list it, whatever its base: a class
//!   deriving from `InvalidOperationException`, from `FormatException` or from
//!   a class of the program's own is a failure where its author says so, and an
//!   arm naming its base still takes it.
//! cites: 30.14, 31.4
//! expect: run
//! stdout: True
//! issue: 8418
module Conf;

import System.Threading;

public class StaleOrder(message: string): InvalidOperationException(message), IFailure
end StaleOrder;

public class Program
  public static method Load(): int
    throw new StaleOrder("order changed");
  end Load;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Load);
    try
      var n := await t;
    catch
      var f: IFailure => Console.WriteLine(f is DomainFailedException)
    end;
  end Main;
end Program;
