//! case: s5_1_0911
//! doc: spec
//! section: 5.1
//! title: a uint argument at a (int) -> int function value is rejected
//! rule: Between signed and unsigned types of the same or narrower width (e.g. `int → uint`, `uint →
//!   int`, `short → ushort`).
//! cites: 27.7
//! expect: reject
//! diagnostic: never implicit
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (int) -> int := lambda(x: int) => x;
    var i: uint := 7u;
    Console.WriteLine(g(i));
  end Main;
end Program;
