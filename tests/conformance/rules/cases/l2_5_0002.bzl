//! case: l2_5_0002
//! doc: stdlib
//! section: 2.5
//! title: an invalid DateTime component raises ArgumentOutOfRangeException
//! rule: The DateTime constructors raise ArgumentOutOfRangeException on
//!   invalid components, such as a month outside 1..12.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      var d := new DateTime(2024, 13, 1);
      Console.WriteLine("no exception");
    catch
      ArgumentOutOfRangeException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
