//! case: l2_5_0008
//! doc: stdlib
//! section: 2.5
//! title: adding past MaxValue raises OverflowException
//! rule: DateTime's Add* family raises OverflowException when the result
//!   falls outside MinValue..MaxValue.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      _ := DateTime.MaxValue.AddDays(1.0);
      Console.WriteLine("no exception");
    catch
      OverflowException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
