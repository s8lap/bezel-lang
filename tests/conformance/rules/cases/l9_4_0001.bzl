//! case: l9_4_0001
//! doc: stdlib
//! section: 9.4
//! title: bool.Parse is case-insensitive
//! rule: static pure method Parse(s: string): bool; // "true"/"false",
//!   case-insensitive
//! cites: 9.4
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(bool.Parse("TRUE"));
  end Main;
end Program;
