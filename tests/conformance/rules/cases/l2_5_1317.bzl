//! case: l2_5_1317
//! doc: stdlib
//! section: 2.5
//! title: DateTime.AddDays and AddMinutes raise OverflowException past the range at double and float
//!   arguments
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
      Console.WriteLine(DateTime.MinValue.AddDays(3652059.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MinValue.AddMinutes(1.0 / 0.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      var f: float := 1.0e30f; Console.WriteLine(DateTime.MinValue.AddDays(f).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
