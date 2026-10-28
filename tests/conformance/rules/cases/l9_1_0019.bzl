//! case: l9_1_0019
//! doc: stdlib
//! section: 9.1
//! title: the radix parse and render are inverse for every integer type
//! rule: stdlib §9.1 (v0.345) "The radix alphabet": "The two are therefore inverse:
//!   `T.Parse(n.ToString(radix), radix)` is `n` for every value of every integer type and every
//!   admitted radix."
//! cites: 9.1
//! expect: run
//! stdout: 255
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(uint.Parse("ff", 16));
  end Main;
end Program;
