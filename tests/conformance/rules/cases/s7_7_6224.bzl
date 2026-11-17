//! case: s7_7_6224
//! doc: spec
//! section: 7.7
//! title: a lambda over a nullable element type may coalesce its parameter
//!   before the operator
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 7.7, 7.11, 22.6
//! expect: run
//! stdout: 2
module Conf;

public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;

public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;

  public static method M(a: int, b: int, c: int, d: int): int
    return a + b + c + d;
  end M;

  public static method Select<T, R>(xs: Array<T>, f: (T) -> R): owned Array<R>
    var r := new Array<R>();
    for var x in xs do r.Add(f(x)); end;
    return r;
  end Select;

  public static method Map<T>(v: T, f: (T) -> string): string
    return f(v);
  end Map;
end Seq;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40)];
    var ns := new Array<int?>();
    ns.Add(3);
    ns.Add(null);
    var r := Seq.Select(ns, lambda(v) => v ?? -1);
    Console.WriteLine(r[0] + r[1]);
  end Main;
end Program;
