//! case: s7_7_0015
//! doc: spec
//! section: 7.7
//! title: a byte right operand of ?? into an int? left operand is widened, not sign-extended
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§5.1])
//!   "to `T` or to `T?`; the result type is `T` when the right operand is non-nullable". "Widening from unsigned to a strictly larger signed type (e.g. `byte → short`, `ushort → int`) is permitted because no value is lost" ([§5.1]), so the `byte` value 200 is the `int` 200.
//! cites: 7.7, 5.1
//! expect: run
//! stdout: 200
//! issue: 6675
module Conf;

public class Program
  public static method Main()
    var a: int? := null;
    var d: byte := 200;
    Console.WriteLine(a ?? d);
  end Main;
end Program;
