//! case: s14_5_4506
//! doc: spec
//! section: 14.5
//! title: an untyped lambda in first position is typed from a seed and a collection
//!   that follow it
//! rule: argument in argument order: the bindings established so far are substituted
//! cites: 22.5
//! expect: run
//! output: 112
//! issue: 8394
module Conf;

public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;

public static class Seq
  public static method Fold3<T, A>(f: (A, T) -> A, seed: A, xs: Array<T>): A
    var acc := seed;
    for var x in xs do acc := f(acc, x); end;
    return acc;
  end Fold3;
end Seq;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.Fold3(lambda(a, v) => a + v.Name.Length, 100, xs));
  end Main;
end Program;
