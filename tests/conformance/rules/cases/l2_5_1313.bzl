//! case: l2_5_1313
//! doc: stdlib
//! section: 2.5
//! title: DateTime.AddYears raises OverflowException past year 9999 and before year 1
//! rule: raise `OverflowException` when the result falls outside `MinValue..MaxValue`
//! cites: 2.5
//! expect: run
//! output: OVF
//!   OVF
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(new DateTime(9999, 12, 31).AddYears(1).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(new DateTime(1, 6, 1).AddYears(-1).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
