//! case: s22_16_0032
//! doc: spec
//! section: 22.16
//! title: a requires violation is a defect: it passes an IFailure arm
//! rule: Every other exception is a defect report: the program is wrong, and the
//!   exception says where.
//! cites: 22.16, 31.4
//! expect: run
//! stdout: defect passed
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
      ContractViolationException => Console.WriteLine("defect passed")
    end;
  end Main;
end Program;
