//! case: l2_5_1307
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan * 1.0 raises OverflowException at MaxValue and returns MinValue's ticks at MinValue
//! rule: The range check reads the double
//! cites: 2.5
//! expect: run
//! output: OVF
//!   -9223372036854775808
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine((TimeSpan.MaxValue * 1.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
    try
      Console.WriteLine((TimeSpan.MinValue * 1.0).Ticks);
    catch OverflowException =>
      Console.WriteLine("OVF");
    end;
  end Main;
end Program;
