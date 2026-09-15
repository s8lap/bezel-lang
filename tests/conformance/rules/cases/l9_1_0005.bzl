//! case: l9_1_0005
//! doc: stdlib
//! section: 9.1
//! title: int.Parse(s, radix) parses a hexadecimal literal
//! rule: static pure method Parse(s: string, radix: int): int; radix in
//!   {2, 8, 10, 16}.
//! cites: 9.1
//! expect: run
//! stdout: 255
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("FF", 16));
  end Main;
end Program;
