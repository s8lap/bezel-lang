//! case: l2_6_0002
//! doc: stdlib
//! section: 2.6
//! title: Parse raises FormatException on a malformed string
//! rule: Guid.Parse(s) accepts the "D" and "N" renderings; FormatException
//!   otherwise.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      _ := Guid.Parse("not-a-guid");
      Console.WriteLine("no exception");
    catch
      FormatException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
