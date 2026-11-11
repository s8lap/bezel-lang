//! case: s14_5_4810
//! doc: spec
//! section: 14.5
//! title: a seed binding K to string disagrees with a later lambda returning int
//! rule: All bindings a type parameter receives
//! cites: 14.5
//! expect: reject
//! diagnostic: cannot be bound to both
module Conf;

record Box(Name: string, Age: int);

public static class Seq
  public static method M<T, K>(xs: Array<T>, seed: K?, key: (T) -> K): T where K: IComparable<K>
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
    Console.WriteLine(Seq.M(xs, "x", lambda(v) => v.Age).Name);
  end Main;
end Program;
