//! case: l2_2_0004
//! doc: stdlib
//! section: 2.2
//! title: Sign yields 0 for NaN rather than -1 or 1
//! rule: Math.Sign<T>(x) is two < tests against T.Zero(), so a NaN operand
//!   yields 0.
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Sign(double.NaN));
  end Main;
end Program;
