//! case: l2_5_1305
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan.FromMilliseconds truncates a fractional tick count toward zero
//! rule: computed over ticks in `double` and truncated toward zero to a whole number of ticks
//! cites: 2.5
//! expect: run
//! output: 1
//!   -1
//!   19999
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(TimeSpan.FromMilliseconds(0.00019).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(TimeSpan.FromMilliseconds(-0.00019).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(TimeSpan.FromMilliseconds(1.99999).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
