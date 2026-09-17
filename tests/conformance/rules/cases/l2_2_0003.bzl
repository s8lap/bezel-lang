//! case: l2_2_0003
//! doc: stdlib
//! section: 2.2
//! title: DivRem raises DivideByZeroException when the divisor is zero
//! rule: Math.DivRem(a, b, out remainder) raises DivideByZeroException when
//!   b = 0, the same fault the / operator raises.
//! cites: 6.1
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      var q := Math.DivRem(10, 0, out var remainder);
      Console.WriteLine("no exception");
    catch
      DivideByZeroException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
