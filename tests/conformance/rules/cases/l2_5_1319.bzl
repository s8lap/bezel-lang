//! case: l2_5_1319
//! doc: stdlib
//! section: 2.5
//! title: DateTime.AddYears and AddMonths raise OverflowException at int extremes instead of wrapping
//! rule: raise `OverflowException` when the result falls outside `MinValue..MaxValue`
//! cites: 2.5
//! expect: run
//! output: OVF
//!   OVF
//!   OVF
//!   OVF
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(new DateTime(2024, 1, 1).AddYears(2147483647).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(new DateTime(2024, 1, 1).AddYears(-2147483647 - 1).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(new DateTime(2024, 1, 1).AddMonths(2147483647).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(new DateTime(2024, 1, 1).AddMonths(-2147483647 - 1).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
