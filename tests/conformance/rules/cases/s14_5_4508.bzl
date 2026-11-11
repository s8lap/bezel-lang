//! case: s14_5_4508
//! doc: spec
//! section: 14.5
//! title: a first-position lambda whose body is a generic call with an untyped lambda
//!   is typed from the array
//! rule: argument in argument order: the bindings established so far are substituted
//! cites: 22.5
//! expect: run
//! output: 32
//!   42
//!   27
//!   37
//! issue: 8394
module Conf;

public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;

public static class Seq
  public static method Sel<T, R>(f: (T) -> R, xs: Array<T>): owned Array<R>
    var r := new Array<R>();
    for var x in xs do r.Add(f(x)); end;
    return r;
  end Sel;
end Seq;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    var ys := new Array<int>();
    ys.Add(1); ys.Add(2);
    var s := Seq.Sel(lambda(v) => Seq.Sel(lambda(w) => w + v.Age, ys).Count + v.Age, xs);
    for var e in s do Console.WriteLine(e); end;
  end Main;
end Program;
