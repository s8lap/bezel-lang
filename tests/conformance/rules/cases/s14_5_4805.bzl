//! case: s14_5_4805
//! doc: spec
//! section: 14.5
//! title: the key type bound from the lambda return is the string itself: a Substring key picks bob
//! rule: the lambda's inferred return type then unifies against the function type's return position, binding type parameters still free
//! cites: 14.5, 22.6
//! expect: run
//! stdout: bob
module Conf;

record Box(Name: string, Age: int);

public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K, seed: K?): T where K: IComparable<K>
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
    Console.WriteLine(Seq.M(xs, lambda(v) => v.Name.Substring(1), null).Name);
  end Main;
end Program;
