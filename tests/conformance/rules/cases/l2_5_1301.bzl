//! case: l2_5_1301
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan.FromDays is in range at 10675199 days and raises OverflowException at 10675200
//! rule: raising `OverflowException` when the result falls outside the tick range or the argument is
//!   `NaN`
//! cites: 2.5
//! expect: run
//! output: 9223371936000000000
//!   OVF
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(TimeSpan.FromDays(10675199.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(TimeSpan.FromDays(10675200.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
