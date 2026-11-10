//! case: s30_14_0017
//! doc: spec
//! section: 30.14
//! title: a failure raised in a child is joined as DomainFailedException
//! rule: where the original exception's class implements `IFailure` (§31.4), the
//!   join raises `DomainFailedException`, which derives from
//!   `DomainFaultedException` and implements `IFailure` (stdlib §6.8). An arm
//!   naming `DomainFaultedException` takes both; an arm naming `IFailure` takes
//!   a failure raised in another domain as it takes one raised in its own.
//! cites: 30.14, 31.4
//! expect: run
//! stdout: True
//! issue: 8418
module Conf;

import System.Threading;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Program
  public static method Load(): int
    throw new ConfigError("bad config");
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
