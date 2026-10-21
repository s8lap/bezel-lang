//! case: l2_2_0007
//! doc: stdlib
//! section: 2.2
//! title: Floor of a negative decimal rounds toward negative infinity
//! rule: The declaration "public static pure method Floor(x: decimal): decimal;" rounds "toward
//!   negative infinity", each result an "integral value; exact, never faults". Floor(-1.5m) is -2.
//! expect: run
//! stdout: -2
//! issue: 6297
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Floor(-1.5m));
  end Main;
end Program;
