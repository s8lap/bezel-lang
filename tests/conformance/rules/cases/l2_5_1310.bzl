//! case: l2_5_1310
//! doc: stdlib
//! section: 2.5
//! title: DateTime.MinValue.Add reaches MaxValue exactly and raises OverflowException one tick further
//! rule: raise `OverflowException` when the result falls outside `MinValue..MaxValue`
//! cites: 2.5
//! expect: run
//! output: 3155378975999999999
//!   OVF
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(DateTime.MinValue.Add(TimeSpan.FromTicks(3155378975999999999L)).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(DateTime.MinValue.Add(TimeSpan.FromTicks(3155378976000000000L)).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
