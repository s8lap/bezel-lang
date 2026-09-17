//! case: s6_1_0001
//! doc: spec
//! section: 6.1
//! title: integer addition whose mathematical result does not fit the result type raises OverflowException
//! rule: Operators +, -, *, /, mod, unary -, ++, and -- on integer types are
//!   checked for overflow. An operation whose mathematical result is not
//!   representable in the result type raises OverflowException.
//! cites: 6.1
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var a: int := 2_000_000_000;
      var b: int := 2_000_000_000;
      var c := a + b;
      Console.WriteLine(c);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
