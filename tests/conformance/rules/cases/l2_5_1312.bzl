//! case: l2_5_1312
//! doc: stdlib
//! section: 2.5
//! title: subtracting MaxValue's tick count from DateTime.MaxValue lands exactly on MinValue
//! rule: raise `OverflowException` when the result falls outside `MinValue..MaxValue`
//! cites: 2.5
//! expect: run
//! output: 0
//!   0
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(DateTime.MaxValue.Subtract(TimeSpan.FromTicks(3155378975999999999L)).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MaxValue.Add(TimeSpan.FromTicks(-3155378975999999999L)).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
