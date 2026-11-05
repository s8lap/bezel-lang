//! case: l2_5_1318
//! doc: stdlib
//! section: 2.5
//! title: DateTime.Add* family converts int and double arguments to the stated tick offsets
//! rule: raise `OverflowException` when the result falls outside `MinValue..MaxValue`
//! cites: 2.5
//! expect: run
//! output: 2592000000000
//!   180000000000
//!   3155378112000000000
//!   10000
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(DateTime.MinValue.AddDays(3).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MinValue.AddHours(5).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MinValue.AddDays(3652058.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MinValue.AddMilliseconds(1.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
