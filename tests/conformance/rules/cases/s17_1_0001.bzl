//! case: s17_1_0001
//! doc: spec
//! section: 17.1
//! title: multiplicative operators bind tighter than additive operators
//! rule: Precedence table: Multiplicative (`*` `/` `mod`) is level 3, Additive
//!   (`+` `-`) is level 4 — the lower-numbered level binds tighter, so `*`
//!   groups before `+`.
//! cites: 17.1
//! expect: run
//! stdout: 14
module Conf;

public class Program
  public static method Main()
    var r := 2 + 3 * 4;
    Console.WriteLine(r);
  end Main;
end Program;
