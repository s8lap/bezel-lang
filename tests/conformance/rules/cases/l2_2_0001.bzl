//! case: l2_2_0001
//! doc: stdlib
//! section: 2.2
//! title: Round uses banker's rounding at a midpoint
//! rule: Math.Round(x, digits) rounds using banker's rounding; a midpoint such
//!   as 2.5 rounds to the nearest even integer, 2, not 3.
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Round(2.5));
  end Main;
end Program;
