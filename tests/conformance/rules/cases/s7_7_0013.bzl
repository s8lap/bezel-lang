//! case: s7_7_0013
//! doc: spec
//! section: 7.7
//! title: a byte right operand of ?? into a short? left operand widens to short
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§5.1])
//!   "to `T` or to `T?`". "Widening from unsigned to a strictly larger signed type (e.g. `byte → short`, `ushort → int`) is permitted because no value is lost" ([§5.1]).
//! cites: 7.7, 5.1
//! expect: run
//! output: 9
//!   3
//! issue: 6214
module Conf;

public class Program
  public static method Main()
    var a: short? := null;
    var d: byte := 9;
    Console.WriteLine(int(a ?? d));
    var b: short? := 3;
    Console.WriteLine(int(b ?? d));
  end Main;
end Program;
