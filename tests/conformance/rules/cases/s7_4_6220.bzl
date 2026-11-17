//! case: s7_4_6220
//! doc: spec
//! section: 7.4
//! title: a lambda parameter bound to a nullable local initialised null is a
//!   nullable value
//! rule: Member access, method calls, and indexing on a nullable value are
//!   compile errors unless the compiler has narrowed it to non-null:
//! cites: 7.4, 22.6
//! expect: reject
//! diagnostic: narrowing
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
    var x: int? := null;
    Console.WriteLine(Seq.Map(x, lambda(y) => y.ToString()));
  end Main;
end Program;
