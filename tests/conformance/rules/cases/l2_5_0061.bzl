//! case: l2_5_0061
//! doc: stdlib
//! section: 2.5
//! title: a DateTime range overflow is a defect: it passes an IFailure arm
//! rule: a band-1 defect of the arithmetic domain (§1), raised with the arithmetic
//!   class and not with an argument-blaming one
//! cites: 2.5, 1
//! expect: run
//! stdout: False
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    try
      try
        _ := DateTime.MaxValue.AddDays(1.0);
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      var ex: OverflowException => Console.WriteLine(ex is ParseRangeException)
    end;
  end Main;
end Program;
