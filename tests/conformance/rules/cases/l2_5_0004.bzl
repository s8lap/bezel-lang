//! case: l2_5_0004
//! doc: stdlib
//! section: 2.5
//! title: Parse raises FormatException on a non-conforming string
//! rule: DateTime.Parse accepts exactly the round-trip form, its fractional-
//!   seconds extension, and the date-only form; any other input raises
//!   FormatException.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      _ := DateTime.Parse("15/03/2024");
      Console.WriteLine("no exception");
    catch
      FormatException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
