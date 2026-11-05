//! case: l2_5_1202
//! doc: stdlib
//! section: 2.5
//! title:DateTime minus DateTime is a TimeSpan; DateTime with a TimeSpan is a DateTime in either
//!   order
//! rule:public static method operator +(t: TimeSpan, d: DateTime): DateTime;
//! cites: 25.2
//! expect: run
//! output: 0
//!   107
//!   107
//!   93
module Conf;

public class Program
  public static method Main()
    var d := DateTime.FromTicks(100L);
    var t := TimeSpan.FromTicks(7L);
    Console.WriteLine((d - d).Ticks);
    Console.WriteLine((d + t).Ticks);
    Console.WriteLine((t + d).Ticks);
    Console.WriteLine((d - t).Ticks);
  end Main;
end Program;
