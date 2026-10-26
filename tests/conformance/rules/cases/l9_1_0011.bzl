//! case: l9_1_0011
//! doc: stdlib
//! section: 9.1
//! title: Parse(s, radix) accepts either case of the letter digits
//! rule: stdlib §9.1: "`Parse(s, radix)` and `TryParse(s, radix, …)` accept
//!     **either case** of the letter digits — `int.Parse("ff", 16)` and
//!     `int.Parse("FF", 16)` are both `255`".
//! cites: 9.1
//! expect: run
//! output: 255
//!   255
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("ff", 16));
    Console.WriteLine(int.Parse("FF", 16));
  end Main;
end Program;
