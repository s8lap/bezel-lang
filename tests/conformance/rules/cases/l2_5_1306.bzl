//! case: l2_5_1306
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan.From<Unit> widens an int argument and yields zero ticks for a negative zero
//! rule: computed over ticks in `double` and truncated toward zero to a whole number of ticks
//! cites: 2.5
//! expect: run
//! output: 2592000000000
//!   0
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(TimeSpan.FromDays(3).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(TimeSpan.FromSeconds(-0.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
