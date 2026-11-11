//! case: s14_5_4505
//! doc: spec
//! section: 14.5
//! title: an untyped lambda placed before the argument binding its accumulator type is
//!   typed after it
//! rule: argument in argument order: the bindings established so far are substituted
//! cites: 22.5
//! expect: run
//! output: 130
//! issue: 8394
module Conf;

public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;

public static class Seq
  public static method Fold2<T, A>(xs: Array<T>, f: (A, T) -> A, seed: A): A
    var acc := seed;
    for var x in xs do acc := f(acc, x); end;
    return acc;
  end Fold2;
end Seq;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.Fold2(xs, lambda(a, v) => a + v.Age, 0));
  end Main;
end Program;
