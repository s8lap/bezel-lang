//! case: s22_16_0032
//! doc: spec
//! section: 22.16
//! title: a requires violation passes an IFailure arm
//! rule: A class that does not implement `IFailure` makes no statement: its
//!   condition may come from the program, from data the program was given, or
//!   from anywhere else, and only the code that handles it knows which. … The
//!   marker is read in two places. A catch arm that names it selects on it: an
//!   `IFailure` arm takes every failure and lets every other exception,
//!   cancellation included, pass
//! cites: 22.16, 31.4
//! expect: run
//! stdout: violation passed
//! issue: 8414
module Conf;

public class Port
  public static method Check(n: int): int
    requires(n > 0)

    return n;
  end Check;
end Port;

public class Program
  public static method Main()
    try
      try
        var zero := 0;
        _ := Port.Check(zero);
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      ContractViolationException => Console.WriteLine("violation passed")
    end;
  end Main;
end Program;
