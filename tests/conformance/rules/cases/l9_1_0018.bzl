//! case: l9_1_0018
//! doc: stdlib
//! section: 9.1
//! title: Parse(s, radix) rejects leading whitespace
//! rule: stdlib §9.1: the input grammar of the radix forms is "an optional
//!     leading `+` or `-`, then one or more digits of that radix, and nothing
//!     else; no whitespace, no `0x`/`0b` prefix, no digit separators, and any
//!     other input raises `FormatException`".
//! cites: 9.1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := int.Parse(" 10", 10);
    catch
      FormatException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
