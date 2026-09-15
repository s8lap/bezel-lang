//! case: s17_1_0003
//! doc: spec
//! section: 17.1
//! title: unary not binds tighter than logical and
//! rule: Precedence table: Unary (`not`, prefix) is level 2, Logical AND
//!   (`and`) is level 13 — the lower-numbered level binds tighter, so
//!   `not a and b` parses as `(not a) and b`, not `not (a and b)`.
//! cites: 17.1
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    var r := not true and false;
    Console.WriteLine(r);
  end Main;
end Program;
