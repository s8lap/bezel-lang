//! case: l6_8_4302
//! doc: stdlib
//! section: 6.8
//! title: FaultStackTrace is inherited by DomainFailedException and readable on a failure caught at a join
//! rule: `DomainFailedException` declares nothing of its own. It derives from `DomainFaultedException`
//! cites: 6.8, 30.14
//! expect: run
//! output: True
//!   bad config
//! issue: 8399
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
      var ex: DomainFailedException => do
        var st: string := ex.FaultStackTrace;
        Console.WriteLine(st.Length >= 0);
        Console.WriteLine(ex.Message);
      end,
    end try;
  end Main;
end Program;
