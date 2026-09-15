//! case: s6_1_0005
//! doc: spec
//! section: 6.1
//! title: integer mod by zero raises DivideByZeroException
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
      var r := 7 mod z;
      Console.WriteLine(r);
    catch
      var ex: DivideByZeroException => Console.WriteLine("divzero");
    end;
  end Main;
end Program;
