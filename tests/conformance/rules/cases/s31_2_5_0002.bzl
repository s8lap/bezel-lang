//! case: s31_2_5_0002
//! doc: spec
//! section: 31.2.5
//! title: IFailure => () suppresses a failure and lets an unmarked exception propagate
//! rule: To suppress only what the environment or a peer raised, name the
//!   marker — `IFailure => ()` — and every other exception, cancellation
//!   included, still propagates (§31.4).
//! cites: 31.2.5, 31.4
//! expect: run
//! output: failure suppressed
//!   unmarked propagated
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
      InvalidOperationException => Console.WriteLine("unmarked propagated")
    end;
  end Main;
end Program;
