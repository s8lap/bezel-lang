//! case: l2_5_0051
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan.MaxValue * 1.0 raises OverflowException
//! rule: The range check reads the double product, so MaxValue * 1.0 raises
//!   (its Ticks convert to 2^63).
//! cites: 2.5
//! expect: run
//! stdout: threw
//! issue: 7608
module Conf;

public class Program
  public static method Main()
    try
      var r := TimeSpan.MaxValue * 1.0;
      Console.WriteLine($"no exception {r.Ticks}");
    catch
      OverflowException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
