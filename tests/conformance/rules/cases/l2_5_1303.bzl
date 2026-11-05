//! case: l2_5_1303
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan.FromMilliseconds raises OverflowException on a NaN argument
//! rule: raising `OverflowException` when the result falls outside the tick range or the argument is
//!   `NaN`
//! cites: 2.5
//! expect: run
//! output: OVF
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(TimeSpan.FromMilliseconds(0.0 / 0.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
