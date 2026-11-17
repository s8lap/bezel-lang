//! case: s26_5_6214
//! doc: spec
//! section: 26.5
//! title: on an overloaded generic name a lambda body naming a member the
//!   element type lacks is rejected
//! rule: the access names no member and is a compile error at the name, the
//!   diagnostic naming the receiver's static type
//! cites: 26.5, 14.5, 22.6
//! expect: reject
//! diagnostic: Nope
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
    Console.WriteLine(Seq.M(xs, lambda(v) => v.Nope).Name);
  end Main;
end Program;
