//! case: l2_5_1201
//! doc: stdlib
//! section: 2.5
//! title:TimeSpan times double, divided by double, plus, minus and negation compute over ticks
//! rule:public static method operator *(a: TimeSpan, factor: double): TimeSpan;
//! cites: 25.2
//! expect: run
//! output: 10
//!   2
//!   10
//!   0
//!   -5
module Conf;

public class Program
  public static method Main()
    var a := TimeSpan.FromTicks(5L);
    Console.WriteLine((a * 2.0).Ticks);
    Console.WriteLine((a / 2.0).Ticks);
    Console.WriteLine((a + a).Ticks);
    Console.WriteLine((a - a).Ticks);
    Console.WriteLine((-a).Ticks);
  end Main;
end Program;
