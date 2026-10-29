//! case: l9_1_0024
//! doc: stdlib
//! section: 9.1
//! title: the int radix form of the same parse call runs under radix 16
//! rule: stdlib §9.1 (v0.350) declares `public static pure method Parse(s: string, radix: int): int;`
//!   with radix in {2, 8, 10, 16}, so 16 is an admitted radix and "ff" parses to 255.
//! cites: 9.1
//! expect: run
//! stdout: 255
//! notes: the accepting twin of l9_1_0023: the one edit is the string radix "16" becoming 16.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("ff", 16));
  end Main;
end Program;
