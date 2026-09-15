//! case: s31_4_0001
//! doc: spec
//! section: 31.4
//! title: an ArithmeticException arm catches an OverflowException
//! rule: The inheritance hierarchy is relevant for arm ordering: an arm
//!   pattern of `ArithmeticException` matches both `OverflowException` and
//!   `DivideByZeroException`.
//! cites: 31.4, 6.1
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
      ArithmeticException => Console.WriteLine("overflow"),
    end try;
  end Main;
end Program;
