//! case: s31_4_0026
//! doc: spec
//! section: 31.4
//! title: a marked subclass of InvalidOperationException is taken by an IFailure arm and by its base's arm
//! rule: Every other `Exception`-derived class may list it, whatever its base:
//!   a class deriving from `InvalidOperationException`, from `FormatException`
//!   or from a class of the program's own is a failure where its author says
//!   so, and an arm naming its base still takes it. … The marker is read in two
//!   places. A catch arm that names it selects on it: an `IFailure` arm takes
//!   every failure and lets every other exception, cancellation included, pass
//! cites: 31.4, 31.2.1
//! expect: run
//! output: marked taken
//!   base taken
//!   unmarked passed
//! issue: 8416
module Conf;

public class StaleOrder(message: string): InvalidOperationException(message), IFailure
end StaleOrder;

public class Program
  public static method Raise(kind: int)
    if kind = 1 then
      throw new StaleOrder("order changed");
    end if;
    throw new InvalidOperationException("bad state");
  end Raise;

  public static method Main()
    try
      Program.Raise(1);
    catch
      IFailure => Console.WriteLine("marked taken")
    end;
    try
      Program.Raise(1);
    catch
      InvalidOperationException => Console.WriteLine("base taken")
    end;
    try
      try
        Program.Raise(2);
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      InvalidOperationException => Console.WriteLine("unmarked passed")
    end;
  end Main;
end Program;
