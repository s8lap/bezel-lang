//! case: l2_2_0008
//! doc: stdlib
//! section: 2.2
//! title: Ceiling of a negative decimal rounds toward positive infinity
//! rule: The declaration "public static pure method Ceiling(x: decimal): decimal;" rounds "toward
//!   positive infinity", each result an "integral value; exact, never faults". Ceiling(-1.5m) is
//!   -1.
//! expect: run
//! stdout: -1
//! issue: 6297
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Ceiling(-1.5m));
  end Main;
end Program;
