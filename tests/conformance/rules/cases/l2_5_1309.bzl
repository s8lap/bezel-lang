//! case: l2_5_1309
//! doc: stdlib
//! section: 2.5
//! title: DateTime.Add and Subtract are in range one tick inside MaxValue and one tick inside MinValue
//! rule: raise `OverflowException` when the result falls outside `MinValue..MaxValue`
//! cites: 2.5
//! expect: run
//! output: 3155378975999999998
//!   3155378975999999998
//!   1
//!   1
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(DateTime.MaxValue.Subtract(TimeSpan.FromTicks(1)).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MaxValue.Add(TimeSpan.FromTicks(-1)).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MinValue.Add(TimeSpan.FromTicks(1)).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MinValue.Subtract(TimeSpan.FromTicks(-1)).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
