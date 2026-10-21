//! case: s5_1_0122
//! doc: spec
//! section: 5.1
//! title: a decimal result binds to a decimal local without conversion
//! rule: "every type converts to itself" ([§5.1]), and "public static pure method Abs(x: decimal):
//!   decimal;" (stdlib §2.2) returns a decimal, so the `decimal` binding is accepted. The product is
//!   exact ("The result of `+`, `-`, `*`, and `/` is the exact result when" [§6.4]), and the stdlib
//!   fixes its rendering: "`0m` renders `0`" (stdlib §9.3), so the printed line is `0`.
//! cites: 5.1, 6.4
//! expect: run
//! stdout: 0
//! issue: 6297
module Conf;

public class Program
  public static method Main()
    var n: decimal := Math.Abs(-2.5m);
    Console.WriteLine(n * 0m);
  end Main;
end Program;
