//! case: s31_2_1_0012
//! doc: spec
//! section: 31.2.1
//! title: a binding alternative listing IFailure binds an Exception
//! rule: an interface listed in a binding alternative counts as `Exception` in the
//!   nearest-common-base computation
//! cites: 31.2.1
//! expect: run
//! output: bad config
//!   bad state
//! issue: 8415
module Conf;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Program
  public static method Raise(kind: int)
    if kind = 1 then
      throw new ConfigError("bad config");
    end if;
    throw new InvalidOperationException("bad state");
  end Raise;

  public static method Main()
    for var kind in 1..2 do
      try
        Program.Raise(kind);
      catch
        var ex: InvalidOperationException | IFailure => Console.WriteLine(ex.Message)
      end;
    end for;
  end Main;
end Program;
