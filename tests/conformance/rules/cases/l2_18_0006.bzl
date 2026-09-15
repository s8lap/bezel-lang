//! case: l2_18_0006
//! doc: stdlib
//! section: 2.18
//! title: decimal F2 rounds a midpoint half-to-even
//! rule: For a decimal operand, digit reduction under F/Fn rounds a
//!   midpoint half-to-even (banker's rounding): 0.125m under F2 renders
//!   "0.12".
//! cites: 2.18
//! expect: run
//! stdout: 0.12
module Conf;

public class Program
  public static method Main()
    var x := 0.125m;
    Console.WriteLine($"{x:F2}");
  end Main;
end Program;
