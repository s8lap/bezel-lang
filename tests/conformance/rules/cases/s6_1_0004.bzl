//! case: s6_1_0004
//! doc: spec
//! section: 6.1
//! title: integer division by zero raises DivideByZeroException
//! rule: Division and mod raise DivideByZeroException when the right operand
//!   is zero.
//! cites: 6.1
//! expect: run
//! stdout: divzero
module Conf;

public class Program
  public static method Main()
    try
      var z: int := 0;
      var q := 7 / z;
      Console.WriteLine(q);
    catch
      var ex: DivideByZeroException => Console.WriteLine("divzero");
    end;
  end Main;
end Program;
