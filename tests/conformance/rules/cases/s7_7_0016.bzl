//! case: s7_7_0016
//! doc: spec
//! section: 7.7
//! title: a ushort right operand of ?? into an int? left operand is widened, not sign-extended
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§5.1])
//!   "to `T` or to `T?`; the result type is `T` when the right operand is non-nullable". "Widening from unsigned to a strictly larger signed type (e.g. `byte → short`, `ushort → int`) is permitted because no value is lost" ([§5.1]), so the `ushort` value 60000 is the `int` 60000.
//! cites: 7.7, 5.1
//! expect: run
//! stdout: 60000
//! issue: 6675
module Conf;

public class Program
  public static method Main()
    var a: int? := null;
    var d: ushort := 60000;
    Console.WriteLine(a ?? d);
  end Main;
end Program;
