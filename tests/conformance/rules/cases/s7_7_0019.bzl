//! case: s7_7_0019
//! doc: spec
//! section: 7.7
//! title: a coalesce result of type int and of type byte is accepted at TimeOnly's named millisecond parameter
//! rule: the result type is `T` when the right operand is non-nullable and `T?` when it is nullable ... so a narrowed `d: Dog?` is exact at a `Dog` parameter and converts to an `Animal` or a `Dog?` one
//! cites: 7.7, 22.6
//! expect: run
//! output: 4
//!   6
module Conf;

public class Program
  public static method Pick(v: int): byte?
    return byte(v);
  end Pick;

  public static method Main()
    var s: int? := null;
    var x := new TimeOnly(1, 2, 3, millisecond := s ?? 4);
    Console.WriteLine(x.Millisecond);
    var b := Pick(6);
    var y := new TimeOnly(1, 2, 3, millisecond := b ?? 9);
    Console.WriteLine(y.Millisecond);
  end Main;
end Program;
