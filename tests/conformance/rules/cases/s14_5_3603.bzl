//! case: s14_5_3603
//! doc: spec
//! section: 14.5
//! title: an omitted optional K? parameter's default is checked after a lambda binds K
//! rule: An argument that contributed no binding — a bare `null`, a collection literal, and the default
//!   value of an omitted optional parameter — neither blocks inference nor escapes the check: it is
//!   checked at the §22.6 applicability check against the substituted signature
//! cites: 14.5, 15.4, 22.6
//! expect: run
//! stdout: dan
//! issue: 8617
//! notes: `K` is bound to `string` by the lambda's return; the remaining argument is then checked.
module Conf;

public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;

public static class Seq
  public static method MaxByD<T, K>(xs: Array<T>, key: (T) -> K, seed: K? := null): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end MaxByD;
end Seq;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.MaxByD(xs, lambda(v) => v.Name + "!").Name);
  end Main;
end Program;
