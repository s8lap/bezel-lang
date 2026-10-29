//! case: l9_6_0042
//! doc: stdlib
//! section: 9.6
//! title: a call to string.Repeat supplying one argument matches no declared form
//! rule: stdlib §9.6 declares `Repeat(c: char, count: int): string` and `Repeat(unit: string, count: int): string`,
//!   both with no default for count. Language spec §22.5: every parameter that declares no default must be
//!   bound, positionally or by name.
//! cites: 9.6, 22.5
//! expect: reject
//! diagnostic: Repeat
//! notes: the twin is l9_6_0039, which differs by the one legal edit of supplying count.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(string.Repeat("ab"));
  end Main;
end Program;
