//! case: l2_5_1311
//! doc: stdlib
//! section: 2.5
//! title: DateTime Add and Subtract with TimeSpan.MinValue or MaxValue raise OverflowException
//! rule: raise `OverflowException` when the result falls outside `MinValue..MaxValue`
//! cites: 2.5
//! expect: run
//! output: OVF
//!   OVF
//!   OVF
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(DateTime.MaxValue.Add(TimeSpan.MinValue).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MinValue.Subtract(TimeSpan.MaxValue).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MinValue.Subtract(TimeSpan.MinValue).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
