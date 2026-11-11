//! case: s14_5_4812
//! doc: spec
//! section: 14.5
//! title: a lambda returning the element binds K to a type that fails the K: IComparable<K> bound
//! rule: constraint satisfaction is checked after inference completes, on the full substitution, exactly as for a written type-argument list
//! cites: 14.5, 14.2
//! expect: reject
//! diagnostic: constraint
module Conf;

record Box(Name: string, Age: int);

public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K, seed: K? := null): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then
        best := x;
        bk := k;
      end;
    end;
    return best;
  end M;
end Seq;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("dan", 35), new Box("bob", 25), new Box("cho", 40)];
    Console.WriteLine(Seq.M(xs, lambda(v) => v).Name);
  end Main;
end Program;
