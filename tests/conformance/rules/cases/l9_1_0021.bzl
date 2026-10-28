//! case: l9_1_0021
//! doc: stdlib
//! section: 9.1
//! title: the radix parse grammar admits no 0x prefix
//! rule: stdlib §9.1 (v0.345) "The radix alphabet": "an optional leading `+` or `-`, then one
//!   or more digits of that radix, and nothing else; no whitespace, no `0x`/`0b` prefix, no digit
//!   separators, and any other input raises `FormatException`".
//! cites: 9.1
//! expect: run
//! stdout: FORMAT
//! issue: 7491
//! notes: the compiler admits "0x1F" at radix 16 and answers 31 — a class 1 divergence, tracked
//!   as #7491 (the coordinator filed it on 2026-10-28; the radix parse lowers to C's strtoll,
//!   which accepts an optional 0x/0X prefix at base 16).
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(int.Parse("0x1F", 16));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
  end Main;
end Program;
