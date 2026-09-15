//! case: l2_3_0002
//! doc: stdlib
//! section: 2.3
//! title: ToInt32 raises OverflowException when the value does not fit
//! rule: Convert.ToInt32(value) raises OverflowException when the value does
//!   not fit the target type.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      _ := Convert.ToInt32("99999999999");
      Console.WriteLine("no exception");
    catch
      OverflowException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
