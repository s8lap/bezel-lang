//! case: s17_1_0004
//! doc: spec
//! section: 17.1
//! title: bitwise AND binds tighter than bitwise XOR
//! rule: Precedence table: Bitwise AND (`&`) is level 7, Bitwise XOR (`^`) is
//!   level 8 — the lower-numbered level binds tighter, so `a & b ^ c` parses
//!   as `(a & b) ^ c`.
//! cites: 17.1
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static method Main()
    var r := 6 & 3 ^ 5;
    Console.WriteLine(r);
  end Main;
end Program;
