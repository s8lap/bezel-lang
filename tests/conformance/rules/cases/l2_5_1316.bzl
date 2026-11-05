//! case: l2_5_1316
//! doc: stdlib
//! section: 2.5
//! title: DateTime.AddYears(0) and AddMonths(0) at the range edges neither raise nor change the ticks
//! rule: raise `OverflowException` when the result falls outside `MinValue..MaxValue`
//! cites: 2.5
//! expect: run
//! output: 3155378975999990000
//!   3155378975999990000
//!   0
//!   0
module Conf;

public class Program
  public static method Main()
    try
      var d := new DateTime(9999, 12, 31, 23, 59, 59, 999, DateTimeKind.Utc); Console.WriteLine(d.AddYears(0).Ticks); Console.WriteLine(d.AddMonths(0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(new DateTime(1, 1, 1).AddYears(0).Ticks); Console.WriteLine(new DateTime(1, 1, 1).AddMonths(0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
