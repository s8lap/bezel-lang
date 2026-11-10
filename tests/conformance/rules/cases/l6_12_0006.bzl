//! case: l6_12_0006
//! doc: stdlib
//! section: 6.12
//! title: a failure in a Parallel.Map body surfaces as DomainFailedException
//! rule: first fault wins, surfaced at the `await` as `DomainFaultedException`, or
//!   as `DomainFailedException` where that fault is a failure (§6.8)
//! cites: 6.12, 6.8
//! expect: run
//! stdout: True
//! issue: 8418
module Conf;

import System.Threading;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Program
  public static method Check(n: int): int
    if n = 3 then
      throw new ConfigError("bad item");
    end if;
    return n;
  end Check;

  public static async method Main(): Task
    var items: Array<int> := [1, 2, 3, 4];
    try
      var results := await Parallel.Map(items, Program.Check);
      Console.WriteLine(results[0]);
    catch
      var f: IFailure => Console.WriteLine(f is DomainFailedException)
    end;
  end Main;
end Program;
