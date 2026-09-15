//! case: l9_1_0001
//! doc: stdlib
//! section: 9.1
//! title: int.Parse raises FormatException on non-numeric input
//! rule: The ParseOrNull note fixes Parse's own contract: absence of a
//!   valid integer literal in the input raises FormatException (the
//!   FormatException/OverflowException contract Parse establishes).
//! cites: 9.1, 1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := int.Parse("abc");
    catch
      FormatException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
