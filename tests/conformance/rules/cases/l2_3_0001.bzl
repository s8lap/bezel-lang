//! case: l2_3_0001
//! doc: stdlib
//! section: 2.3
//! title: ToInt32 raises FormatException on a non-numeric string
//! rule: Convert.ToInt32(value) raises FormatException when the string is not
//!   a well-formed representation of the target type.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      _ := Convert.ToInt32("not a number");
      Console.WriteLine("no exception");
    catch
      FormatException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
