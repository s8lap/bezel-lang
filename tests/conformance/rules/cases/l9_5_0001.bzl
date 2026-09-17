//! case: l9_5_0001
//! doc: stdlib
//! section: 9.5
//! title: char.Parse raises FormatException on a multi-code-point string
//! rule: Parse accepts a string containing exactly one code point
//!   (s.Length = 1); any other input — the empty string or a
//!   multi-code-point string — raises FormatException.
//! cites: 9.5
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := char.Parse("ab");
    catch
      FormatException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
