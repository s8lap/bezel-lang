//! case: s31_2_5_0002
//! doc: spec
//! section: 31.2.5
//! title: IFailure => () suppresses a failure and lets a defect propagate
//! rule: To suppress expected failures only, name the marker — `IFailure => ()` —
//!   and defects and cancellation still propagate (§31.4).
//! cites: 31.2.5, 31.4
//! expect: run
//! output: failure suppressed
//!   defect propagated
//! issue: 8414
module Conf;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Program
  public static method Run(kind: int)
    try
      if kind = 1 then
        throw new ConfigError("bad config");
      end if;
      throw new InvalidOperationException("bad state");
    catch
      IFailure => ()
    end;
  end Run;

  public static method Main()
    Program.Run(1);
    Console.WriteLine("failure suppressed");
    try
      Program.Run(2);
    catch
      InvalidOperationException => Console.WriteLine("defect propagated")
    end;
  end Main;
end Program;
