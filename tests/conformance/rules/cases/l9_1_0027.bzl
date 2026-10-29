//! case: l9_1_0027
//! doc: stdlib
//! section: 9.1
//! title: at radix 16 the text 0b11 is four hex digits, with no prefix stripped
//! rule: stdlib §9.1 (v0.350) "The radix alphabet": the digits of radix 16 are "`0`–`9` with
//!   `A`–`F`", and the input is "an optional leading `+` or `-`, then one or more digits of that
//!   radix, and nothing else; no whitespace, no `0x`/`0b` prefix". `b` is a radix-16 digit, so
//!   "0b11" is the digits 0, b, 1, 1: 0xB11 = 2833.
//! cites: 9.1
//! expect: run
//! stdout: 2833
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("0b11", 16));
  end Main;
end Program;
