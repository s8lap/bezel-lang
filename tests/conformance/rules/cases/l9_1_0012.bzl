//! case: l9_1_0012
//! doc: stdlib
//! section: 9.1
//! title: Parse(s, radix) raises FormatException on a digit outside the radix
//! rule: stdlib §9.1: the input grammar of the radix forms is "one or more
//!     digits of that radix, and nothing else; no whitespace, no `0x`/`0b`
//!     prefix, no digit separators, and any other input raises
//!     `FormatException`". `9` is not a digit of radix 8.
//! cites: 9.1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := int.Parse("9", 8);
    catch
      FormatException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
