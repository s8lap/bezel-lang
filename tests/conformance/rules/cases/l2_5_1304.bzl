//! case: l2_5_1304
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan.From<Unit> raises OverflowException on an infinite argument of either sign
//! rule: raising `OverflowException` when the result falls outside the tick range or the argument is
//!   `NaN`
//! cites: 2.5
//! expect: run
//! output: OVF
//!   OVF
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(TimeSpan.FromHours(1.0 / 0.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(TimeSpan.FromMinutes(-1.0 / 0.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
