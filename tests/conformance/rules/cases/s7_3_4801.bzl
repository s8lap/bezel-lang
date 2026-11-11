//! case: s7_3_4801
//! doc: spec
//! section: 7.3
//! title: null at a non-nullable K formal after K is bound to string from the lambda is rejected
//! rule: Assigning `null` to a non-nullable target is a compile error.
//! cites: 14.5, 22.6
//! expect: reject
//! diagnostic: null
module Conf;

record Box(Name: string, Age: int);

public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K, seed: K): T where K: IComparable<K>
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
    Console.WriteLine(Seq.M(xs, lambda(v) => v.Name + "!", null).Name);
  end Main;
end Program;
