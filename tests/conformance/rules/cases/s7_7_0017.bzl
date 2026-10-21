//! case: s7_7_0017
//! doc: spec
//! section: 7.7
//! title: a uint right operand of ?? into a long? left operand widens to its full value
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§7.7]).
//!   "Widening from unsigned to a strictly larger signed type" ([§5.1]) "is permitted because no
//!   value is lost" ([§5.1]): the `uint` 3000000000 widens to `long` as 3000000000.
//! cites: 7.7, 5.1
//! expect: run
//! stdout: 3000000000
//! issue: 6675
module Conf;

public class Program
  public static method Main()
    var a: long? := null;
    var d: uint := 3000000000;
    Console.WriteLine(a ?? d);
  end Main;
end Program;
