//! case: s5_1_0122
//! doc: spec
//! section: 5.1
//! title: a decimal result binds to a decimal local without conversion
//! rule: "every type converts to itself" ([§5.1]), and "public static pure method Abs(x: decimal):
//!   decimal;" (stdlib §2.2) returns a decimal, so the `decimal` binding is accepted.
//! cites: 5.1
//! expect: run
//! stdout: 2.5
//! issue: 6297
module Conf;

public class Program
  public static method Main()
    var n: decimal := Math.Abs(-2.5m);
    Console.WriteLine(n);
  end Main;
end Program;
