//! case: l2_5_1315
//! doc: stdlib
//! section: 2.5
//! title: DateTime.AddYears and AddMonths return in-range results at the year 1 and year 9999 edges
//! rule: raise `OverflowException` when the result falls outside `MinValue..MaxValue`
//! cites: 2.5
//! expect: run
//! output: 9999-12-31T00:00:00
//!   9999-01-31T00:00:00
//!   0001-06-01T00:00:00
//!   0001-01-31T00:00:00
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(new DateTime(9999, 1, 31).AddMonths(11).ToString());
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(new DateTime(9999, 12, 31).AddMonths(-11).ToString());
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(new DateTime(2, 6, 1).AddYears(-1).ToString());
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine(new DateTime(1, 12, 31).AddMonths(-11).ToString());
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
