//! case: l9_6_0011
//! doc: stdlib
//! section: 9.6
//! title: string.Repeat with a count of zero is the empty string
//! rule: public static pure method Repeat(c: char, count: int): string;  // count < 0
//!   raises ArgumentOutOfRangeException; count = 0 is ""
//! cites: 9.6
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(string.Repeat('x', 0).Length);
  end Main;
end Program;
