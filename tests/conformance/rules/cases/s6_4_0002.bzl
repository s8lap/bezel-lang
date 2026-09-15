//! case: s6_4_0002
//! doc: spec
//! section: 6.4
//! title: decimal division by zero raises DivideByZeroException
//! rule: decimal arithmetic raises OverflowException on overflow and
//!   DivideByZeroException on division by zero.
//! cites: 6.4
//! expect: run
//! stdout: divzero
module Conf;

public class Program
  public static method Main()
    try
      var z: decimal := 0m;
      var r := 1m / z;
      Console.WriteLine(r);
    catch
      var ex: DivideByZeroException => Console.WriteLine("divzero");
    end;
  end Main;
end Program;
