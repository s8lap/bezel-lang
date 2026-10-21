//! case: l2_2_0009
//! doc: stdlib
//! section: 2.2
//! title: Truncate of a negative decimal rounds toward zero
//! rule: The declaration "public static pure method Truncate(x: decimal): decimal;" is commented
//!   "Truncate is toward zero." Truncate(-1.5m) is -1.
//! expect: run
//! stdout: -1
//! issue: 6297
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Truncate(-1.5m));
  end Main;
end Program;
