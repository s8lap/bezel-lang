//! case: l9_5_0003
//! doc: stdlib
//! section: 9.5
//! title: char.ToUpper applies the simple case mapping to a character
//! rule: public static pure method ToUpper(c: char): char; — `ToUpper`/`ToLower`
//!   apply the simple case mapping — one code point to one, unconditional, no
//!   locale tailoring — and return the argument unchanged where it has none.
//! cites: 9.5
//! expect: run
//! stdout: A
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(char.ToUpper('a'));
  end Main;
end Program;
