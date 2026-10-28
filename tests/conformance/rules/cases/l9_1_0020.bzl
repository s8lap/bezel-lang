//! case: l9_1_0020
//! doc: stdlib
//! section: 9.1
//! title: the radix parse grammar admits no digit separator
//! rule: stdlib §9.1 (v0.345) "The radix alphabet": "an optional leading `+` or `-`, then one
//!   or more digits of that radix, and nothing else; no whitespace, no `0x`/`0b` prefix, no digit
//!   separators, and any other input raises `FormatException`".
//! cites: 9.1
//! expect: run
//! stdout: FORMAT
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(int.Parse("1_000", 10));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
  end Main;
end Program;
