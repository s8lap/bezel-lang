//! case: l9_1_0031
//! doc: stdlib
//! section: 9.1
//! title: an arithmetic overflow passes an IFailure arm
//! rule: the arithmetic `OverflowException` of language spec §6.1 is a defect, and
//!   no arm on `IFailure` takes it.
//! cites: 9.1, 1
//! expect: run
//! stdout: False
//! issue: 8414
module Conf;

public class Program
  public static method Add(a: int, b: int): int => a + b;

  public static method Main()
    try
      try
        Console.WriteLine(Program.Add(int.MaxValue, 1));
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      var ex: OverflowException => Console.WriteLine(ex is ParseRangeException)
    end;
  end Main;
end Program;
