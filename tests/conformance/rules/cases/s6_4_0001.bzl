//! case: s6_4_0001
//! doc: spec
//! section: 6.4
//! title: decimal arithmetic that overflows raises OverflowException
//! rule: decimal arithmetic raises OverflowException on overflow and
//!   DivideByZeroException on division by zero ... There is no Infinity or
//!   NaN for decimal.
//! cites: 6.4
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var m: decimal := decimal.MaxValue;
      var r := m + 1m;
      Console.WriteLine(r);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
