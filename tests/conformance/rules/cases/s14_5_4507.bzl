//! case: s14_5_4507
//! doc: spec
//! section: 14.5
//! title: an untyped two-parameter lambda in first position is typed from two later
//!   collections
//! rule: argument in argument order: the bindings established so far are substituted
//! cites: 22.5
//! expect: run
//! output: ann31
//!   cho42
//!   bob28
//!   dan39
//! issue: 8394
module Conf;

public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;

public static class Seq
  public static method Zip2<A, B, R>(f: (A, B) -> R, xs: Array<A>, ys: Array<B>): owned Array<R>
    var r := new Array<R>();
    var i := 0;
    while i < xs.Count and i < ys.Count do r.Add(f(xs[i], ys[i])); i := i + 1; end;
    return r;
  end Zip2;
end Seq;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    var ys := new Array<int>();
    ys.Add(1); ys.Add(2); ys.Add(3); ys.Add(4);
    var s := Seq.Zip2(lambda(b, n) => $"{b.Name}{n + b.Age}", xs, ys);
    for var e in s do Console.WriteLine(e); end;
  end Main;
end Program;
