//! case: l9_3_0001
//! doc: stdlib
//! section: 9.3
//! title: Decimal.Round uses banker's rounding (round-half-to-even) at a midpoint
//! rule: pure method Round(digits: int): decimal; // banker's rounding
//!   (round-half-to-even), matching Math.Round.
//! cites: 9.3, 2.2
//! expect: run
//! output: True
//!   True
module Conf;

public class Program
  public static method Main()
    var a := 2.5m;
    var b := 3.5m;
    Console.WriteLine(a.Round(0) = 2m);
    Console.WriteLine(b.Round(0) = 4m);
  end Main;
end Program;
