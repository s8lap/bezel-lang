//! case: s17_1_0005
//! doc: spec
//! section: 17.1
//! title: bitwise XOR binds tighter than bitwise OR
//! rule: Precedence table: Bitwise XOR (`^`) is level 8, Bitwise OR (`|`) is
//!   level 9 — the lower-numbered level binds tighter, so `a ^ b | c` parses
//!   as `(a ^ b) | c`.
//! cites: 17.1
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var r := 6 ^ 3 | 5;
    Console.WriteLine(r);
  end Main;
end Program;
