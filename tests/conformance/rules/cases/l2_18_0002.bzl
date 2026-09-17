//! case: l2_18_0002
//! doc: stdlib
//! section: 2.18
//! title: integer X renders uppercase hex with no grouping separators
//! rule: Integers admit X/x hex (upper/lower), no grouping separators (no
//!   culture).
//! cites: 2.18
//! expect: run
//! stdout: FF
module Conf;

public class Program
  public static method Main()
    var n := 255;
    Console.WriteLine($"{n:X}");
  end Main;
end Program;
