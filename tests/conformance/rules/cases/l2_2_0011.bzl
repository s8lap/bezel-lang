//! case: l2_2_0011
//! doc: stdlib
//! section: 2.2
//! title: Max<decimal> over byte operands returns the decimal maximum
//! rule: `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and `b` otherwise ...
//! cites: 2.2, 5.1
//! expect: run
//! stdout: 100
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Max<decimal>(byte(3), byte(100)));
  end Main;
end Program;
