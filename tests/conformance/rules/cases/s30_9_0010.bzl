//! case: s30_9_0010
//! doc: spec
//! section: 30.9
//! title: a task faulted with a failure is taken by an IFailure arm at each await
//! rule: The ownership of a thrown exception (§31.1), the cleanup an exception
//!   edge runs (K3.15), a task's fault (§30.9), the purity carve-out (§22.15),
//!   supersession (§31.2), and the reports of §32.8 and §33.3 are the same for
//!   a failure, for an exception whose class makes no statement, and for
//!   cancellation.
//! cites: 30.9, 31.4
//! expect: run
//! output: awaiter 1: failure
//!   awaiter 2: failure
//! issue: 8414
module Conf;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Program
  public static async method Load(kind: int): Task<int>
    await Task.Delay(1);
    if kind = 1 then
      throw new ConfigError("bad reply");
    end if;
    return kind;
  end Load;

  public static async method Main(): Task
    using var t := Program.Load(1);
    for var i in 1..2 do
      try
        var n := await t;
        Console.WriteLine(n);
      catch
        IFailure => Console.WriteLine($"awaiter {i}: failure")
      end;
    end for;
  end Main;
end Program;
