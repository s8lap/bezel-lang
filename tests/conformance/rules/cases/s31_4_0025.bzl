//! case: s31_4_0025
//! doc: spec
//! section: 31.4
//! title: a user hierarchy under a user base mixes kinds: the IFailure arm takes only the marked class
//! rule: A class whose base chain is user-declared down to `Exception` chooses
//!   freely at each level. … A catch arm that names the marker selects on it:
//!   an `IFailure` arm takes every failure and lets defects and cancellation
//!   pass
//! cites: 31.4, 31.2.1
//! expect: run
//! output: failure
//!   defect
//! issue: 8414
module Conf;

public class AppError(message: string): Exception(message)
end AppError;

public class NotFound(message: string): AppError(message), IFailure
end NotFound;

public class Program
  public static method Raise(kind: int)
    if kind = 1 then
      throw new NotFound("no such order");
    end if;
    throw new AppError("bad state");
  end Raise;

  public static method Probe(kind: int)
    try
      try
        Program.Raise(kind);
      catch
        IFailure => Console.WriteLine("failure")
      end;
    catch
      AppError => Console.WriteLine("defect")
    end;
  end Probe;

  public static method Main()
    Program.Probe(1);
    Program.Probe(2);
  end Main;
end Program;
